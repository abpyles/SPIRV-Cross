/*
 * Copyright 2015-2016 The Brenwill Workshop Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "spirv_msl.hpp"
#include "GLSL.std.450.h"
#include <algorithm>
#include <numeric>

#include <iostream>

using namespace spv;
using namespace spirv_cross;
using namespace std;

CompilerMSL::CompilerMSL(vector<uint32_t> spirv_)
    : CompilerGLSL(move(spirv_))
{
	options.vertex.fixup_clipspace = false;

	populate_func_name_overrides();
}

// Populate the collection of function names that need to be overridden
void CompilerMSL::populate_func_name_overrides()
{
	func_name_overrides["main"] = "main0";
	func_name_overrides["saturate"] = "saturate0";
}

string CompilerMSL::compile(MSLConfiguration &msl_cfg, vector<MSLVertexAttr> *p_vtx_attrs,
                            std::vector<MSLResourceBinding> *p_res_bindings)
{
	// Force a classic "C" locale, reverts when function returns
	ClassicLocale classic_locale;

	// Remember the input parameters
	msl_config = msl_cfg;

	vtx_attrs_by_location.clear();
	if (p_vtx_attrs)
		for (auto &va : *p_vtx_attrs)
			vtx_attrs_by_location[va.location] = &va;

	resource_bindings.clear();
	if (p_res_bindings)
		for (auto &rb : *p_res_bindings)
			resource_bindings.push_back(&rb);

	// Establish the need to output any custom functions
	set_enabled_interface_variables(get_active_interface_variables());
	register_custom_functions();

	// Create structs to hold input, output and uniform variables
	qual_pos_var_name = "";
	stage_in_var_id = add_interface_block(StorageClassInput);
	stage_out_var_id = add_interface_block(StorageClassOutput);
	stage_uniforms_var_id = add_interface_block(StorageClassUniformConstant);

	// Convert the use of global variables to recursively-passed function parameters
	localize_global_variables();
	extract_global_variables_from_functions();

	// Do not deal with GLES-isms like precision, older extensions and such.
	options.es = false;
	options.version = 120;
	backend.float_literal_suffix = false;
	backend.uint32_t_literal_suffix = true;
	backend.basic_int_type = "int";
	backend.basic_uint_type = "uint";
	backend.discard_literal = "discard_fragment()";
	backend.swizzle_is_function = false;
	backend.shared_is_implied = false;
	backend.native_row_major_matrix = false;

	uint32_t pass_count = 0;
	do
	{
		if (pass_count >= 3)
			SPIRV_CROSS_THROW("Over 3 compilation loops detected. Must be a bug!");

		reset();

		next_metal_resource_index = MSLResourceBinding(); // Start bindings at zero

		// Move constructor for this type is broken on GCC 4.9 ...
		buffer = unique_ptr<ostringstream>(new ostringstream());

		emit_header();
		emit_resources();
		emit_custom_functions();
		emit_function_declarations();
		emit_function(get<SPIRFunction>(entry_point), 0);

		pass_count++;
	} while (force_recompile);

	return buffer->str();
}

string CompilerMSL::compile()
{
	MSLConfiguration default_msl_cfg;
	return compile(default_msl_cfg, nullptr, nullptr);
}

// Register the need to output any custom functions.
void CompilerMSL::register_custom_functions()
{
	custom_function_ops.clear();
	CustomFunctionHandler handler(*this, custom_function_ops);
	traverse_all_reachable_opcodes(get<SPIRFunction>(entry_point), handler);
}

// Move the Private global variables to the entry function.
// Non-constant variables cannot have global scope in Metal.
void CompilerMSL::localize_global_variables()
{
	auto &entry_func = get<SPIRFunction>(entry_point);
	auto iter = global_variables.begin();
	while (iter != global_variables.end())
	{
		uint32_t gv_id = *iter;
		auto &gbl_var = get<SPIRVariable>(gv_id);
		if (gbl_var.storage == StorageClassPrivate)
		{
			entry_func.add_local_variable(gv_id);
			//iter = global_variables.erase(iter);
			iter++;
		}
		else
			iter++;
	}
}

// For any global variable accessed directly by a function,
// extract that variable and add it as an argument to that function.
void CompilerMSL::extract_global_variables_from_functions()
{

	// Uniforms
	std::unordered_set<uint32_t> global_var_ids;
	for (auto &id : ids)
	{
		if (id.get_type() == TypeVariable)
		{
			auto &var = id.get<SPIRVariable>();
			if (var.storage == StorageClassInput || var.storage == StorageClassUniform ||
			    var.storage == StorageClassUniformConstant || var.storage == StorageClassPushConstant)
			{
				global_var_ids.insert(var.self);
			}
		}
	}

	std::unordered_set<uint32_t> added_arg_ids;
	std::unordered_set<uint32_t> processed_func_ids;
	extract_global_variables_from_function(entry_point, added_arg_ids, global_var_ids, processed_func_ids);
}

// MSL does not support the use of global variables for shader input content.
// For any global variable accessed directly by the specified function, extract that variable,
// add it as an argument to that function, and the arg to the added_arg_ids collection.
void CompilerMSL::extract_global_variables_from_function(uint32_t func_id, std::unordered_set<uint32_t> &added_arg_ids,
                                                         std::unordered_set<uint32_t> &global_var_ids,
                                                         std::unordered_set<uint32_t> &processed_func_ids)
{
	// Avoid processing a function more than once
	if (processed_func_ids.find(func_id) != processed_func_ids.end())
	{
		// Return function global variables
		added_arg_ids = function_global_vars[func_id];
		return;
	}

	processed_func_ids.insert(func_id);

	auto &func = get<SPIRFunction>(func_id);

	// Recursively establish global args added to functions on which we depend.
	for (auto block : func.blocks)
	{
		auto &b = get<SPIRBlock>(block);
		for (auto &i : b.ops)
		{
			auto ops = stream(i);
			auto op = static_cast<Op>(i.op);

			switch (op)
			{
			case OpLoad:
			case OpAccessChain:
			{
				uint32_t base_id = ops[2];
				if (global_var_ids.find(base_id) != global_var_ids.end())
					added_arg_ids.insert(base_id);

				if (std::find(global_variables.begin(), global_variables.end(), base_id) != global_variables.end())
					added_arg_ids.insert(base_id);

				break;
			}
			case OpFunctionCall:
			{
				uint32_t inner_func_id = ops[2];
				std::unordered_set<uint32_t> inner_func_args;
				extract_global_variables_from_function(inner_func_id, inner_func_args, global_var_ids,
				                                       processed_func_ids);
				added_arg_ids.insert(inner_func_args.begin(), inner_func_args.end());
				break;
			}

			default:
				break;
			}
		}
	}

	function_global_vars[func_id] = added_arg_ids;

	// Add the global variables as arguments to the function
	if (func_id != entry_point)
	{
		uint32_t next_id = increase_bound_by(uint32_t(added_arg_ids.size()));
		for (uint32_t arg_id : added_arg_ids)
		{
			uint32_t type_id = get<SPIRVariable>(arg_id).basetype;
			func.add_parameter(type_id, next_id);
			set<SPIRVariable>(next_id, type_id, StorageClassFunction);

			// Ensure both the existing and new variables have the same name, and the name is valid
			string vld_name = ensure_valid_name(to_name(arg_id), "v");
			set_name(arg_id, vld_name);
			set_name(next_id, vld_name);

			meta[next_id].decoration.qualified_alias = meta[arg_id].decoration.qualified_alias;
			next_id++;
		}
	}
}

// If a vertex attribute exists at the location, it is marked as being used by this shader
void CompilerMSL::mark_location_as_used_by_shader(uint32_t location, StorageClass storage)
{
	MSLVertexAttr *p_va;
	auto &execution = get_entry_point();
	if ((execution.model == ExecutionModelVertex) && (storage == StorageClassInput) &&
	    (p_va = vtx_attrs_by_location[location]))
		p_va->used_by_shader = true;
}

// Add an interface structure for the type of storage, which is either StorageClassInput or StorageClassOutput.
// Returns the ID of the newly added variable, or zero if no variable was added.
uint32_t CompilerMSL::add_interface_block(StorageClass storage)
{
	// Accumulate the variables that should appear in the interface struct
	vector<SPIRVariable *> vars;
	bool incl_builtins = (storage == StorageClassOutput);
	for (auto &id : ids)
	{
		if (id.get_type() == TypeVariable)
		{
			auto &var = id.get<SPIRVariable>();
			auto &type = get<SPIRType>(var.basetype);
			if (var.storage == storage && interface_variable_exists_in_entry_point(var.self) &&
			    !is_hidden_variable(var, incl_builtins) && type.pointer)
			{
				vars.push_back(&var);
			}
		}
	}

	// If no variables qualify, leave
	if (vars.empty())
		return 0;

	// Add a new typed variable for this interface structure.
	// The initializer expression is allocated here, but populated when the function
	// declaraion is emitted, because it is cleared after each compilation pass.
	uint32_t next_id = increase_bound_by(3);
	uint32_t ib_type_id = next_id++;
	auto &ib_type = set<SPIRType>(ib_type_id);
	ib_type.basetype = SPIRType::Struct;
	ib_type.storage = storage;
	set_decoration(ib_type_id, DecorationBlock);

	uint32_t ib_var_id = next_id++;
	auto &var = set<SPIRVariable>(ib_var_id, ib_type_id, storage, 0);
	var.initializer = next_id++;

	string ib_var_ref;
	switch (storage)
	{
	case StorageClassInput:
		ib_var_ref = stage_in_var_name;
		break;

	case StorageClassOutput:
	{
		ib_var_ref = stage_out_var_name;

		// Add the output interface struct as a local variable to the entry function,
		// and force the entry function to return the output interface struct from
		// any blocks that perform a function return.
		auto &entry_func = get<SPIRFunction>(entry_point);
		entry_func.add_local_variable(ib_var_id);
		for (auto &blk_id : entry_func.blocks)
		{
			auto &blk = get<SPIRBlock>(blk_id);
			if (blk.terminator == SPIRBlock::Return)
				blk.return_value = ib_var_id;
		}
		break;
	}

	case StorageClassUniformConstant:
	{
		ib_var_ref = stage_uniform_var_name;
	}

	default:
		break;
	}

	set_name(ib_type_id, get_entry_point_name() + "_" + ib_var_ref);
	set_name(ib_var_id, ib_var_ref);

	for (auto p_var : vars)
	{
		uint32_t type_id = p_var->basetype;
		auto &type = get<SPIRType>(type_id);
		if (type.basetype == SPIRType::Struct)
		{
			// Flatten the struct members into the interface struct
			uint32_t mbr_idx = 0;
			for (auto &member : type.member_types)
			{
				// Add a reference to the member to the interface struct.
				uint32_t ib_mbr_idx = uint32_t(ib_type.member_types.size());
				ib_type.member_types.push_back(member); // membertype.self is different for array types

				// Give the member a name
				string mbr_name = ensure_valid_name(to_qualified_member_name(type, mbr_idx), "m");
				set_member_name(ib_type_id, ib_mbr_idx, mbr_name);

				// Update the original variable reference to include the structure reference
				string qual_var_name = ib_var_ref + "." + mbr_name;
				set_member_qualified_name(type_id, mbr_idx, qual_var_name);

				// Copy the variable location from the original variable to the member
				uint32_t locn = get_member_decoration(type_id, mbr_idx, DecorationLocation);
				set_member_decoration(ib_type_id, ib_mbr_idx, DecorationLocation, locn);
				mark_location_as_used_by_shader(locn, storage);

				// Mark the member as builtin if needed
				BuiltIn builtin;
				if (is_member_builtin(type, mbr_idx, &builtin))
				{
					set_member_decoration(ib_type_id, ib_mbr_idx, DecorationBuiltIn, builtin);
					if (builtin == BuiltInPosition)
						qual_pos_var_name = qual_var_name;
				}

				mbr_idx++;
			}
		}
		else if (type.basetype == SPIRType::Boolean || type.basetype == SPIRType::Char ||
		         type.basetype == SPIRType::Int || type.basetype == SPIRType::UInt ||
		         type.basetype == SPIRType::Int64 || type.basetype == SPIRType::UInt64 ||
		         type.basetype == SPIRType::Float || type.basetype == SPIRType::Double ||
		         type.basetype == SPIRType::Boolean)
		{
			// Add a reference to the variable type to the interface struct.
			uint32_t ib_mbr_idx = uint32_t(ib_type.member_types.size());
			ib_type.member_types.push_back(type_id);

			// Give the member a name
			string mbr_name = ensure_valid_name(to_expression(p_var->self), "m");
			set_member_name(ib_type_id, ib_mbr_idx, mbr_name);

			// Update the original variable reference to include the structure reference
			string qual_var_name = ib_var_ref + "." + mbr_name;
			meta[p_var->self].decoration.qualified_alias = qual_var_name;

			// Copy the variable location from the original variable to the member
			auto &dec = meta[p_var->self].decoration;
			uint32_t locn = dec.location;
			if (is_decoration_set(p_var->self, DecorationLocation))
			{
				set_member_decoration(ib_type.self, ib_mbr_idx, DecorationLocation, locn);
			}
			mark_location_as_used_by_shader(locn, storage);

			// Mark the member as builtin if needed
			if (is_builtin_variable(*p_var))
			{
				set_member_decoration(ib_type_id, ib_mbr_idx, DecorationBuiltIn, dec.builtin_type);
				is_builtin_variable(*p_var);
				if (dec.builtin_type == BuiltInPosition)
					qual_pos_var_name = qual_var_name;
			}
		}
	}

	// Sort the members of the interface structure by their attribute numbers.
	// Oddly, Metal handles inputs better if they are sorted in reverse order,
	// particularly if the offsets are all equal.
	MemberSorter::SortAspect sort_aspect =
	    (storage == StorageClassInput) ? MemberSorter::LocationReverse : MemberSorter::Location;
	MemberSorter memberSorter(ib_type, meta[ib_type_id], sort_aspect);
	memberSorter.sort();

	// Sort input or output variables alphabetical
	auto &execution = get_entry_point();
	if ((execution.model == ExecutionModelFragment && storage == StorageClassInput) ||
	    (execution.model == ExecutionModelVertex && storage == StorageClassOutput))
	{
		MemberSorter memberSorter(ib_type, meta[ib_type.self], MemberSorter::Alphabetical);
		memberSorter.sort();
	}

	return ib_var_id;
}

// Emits the file header info
void CompilerMSL::emit_header()
{
	for (auto &header : header_lines)
		statement(header);

	statement("#include <metal_stdlib>");
	statement("#include <simd/simd.h>");
	statement("");
	statement("using namespace metal;");
	statement("");
}

// Emits any needed custom function bodies.
void CompilerMSL::emit_custom_functions()
{
	for (auto &op : custom_function_ops)
	{
		switch (op)
		{
		case OpFMod:
			statement("// Support GLSL mod(), which is slightly different than Metal fmod()");
			statement("template<typename Tx, typename Ty>");
			statement("Tx mod(Tx x, Ty y);");
			statement("template<typename Tx, typename Ty>");
			statement("Tx mod(Tx x, Ty y)");
			begin_scope();
			statement("return x - y * floor(x / y);");
			end_scope();
			statement("");
			break;

		default:
			break;
		}
	}
}

void CompilerMSL::emit_resources()
{

	// Output all basic struct types which are not Block or BufferBlock as these are declared inplace
	// when such variables are instantiated.
	for (auto &id : ids)
	{
		if (id.get_type() == TypeType)
		{
			auto &type = id.get<SPIRType>();
			if (type.basetype == SPIRType::Struct && type.array.empty() && !type.pointer &&
			    (meta[type.self].decoration.decoration_flags &
			     ((1ull << DecorationBlock) | (1ull << DecorationBufferBlock))) == 0)
			{
				emit_struct(type);
			}
		}
	}

	// Output Uniform buffers and constants
	for (auto &id : ids)
	{
		if (id.get_type() == TypeVariable)
		{
			auto &var = id.get<SPIRVariable>();
			auto &type = get<SPIRType>(var.basetype);

			if (var.storage != StorageClassFunction && type.pointer &&
			    (type.storage == StorageClassUniform || type.storage == StorageClassUniformConstant ||
			     type.storage == StorageClassPushConstant) &&
			    !is_hidden_variable(var) && (meta[type.self].decoration.decoration_flags &
			                                 ((1ull << DecorationBlock) | (1ull << DecorationBufferBlock))))
			{
				emit_struct(type);
			}
		}
	}

	// Output interface blocks.
	emit_interface_block(stage_in_var_id);
	emit_interface_block(stage_out_var_id);
	emit_interface_block(stage_uniforms_var_id);
}

// Override for MSL-specific syntax instructions
void CompilerMSL::emit_instruction(const Instruction &instruction)
{

#define BOP(op) emit_binary_op(ops[0], ops[1], ops[2], ops[3], #op)
#define BOP_CAST(op, type) \
	emit_binary_op_cast(ops[0], ops[1], ops[2], ops[3], #op, type, opcode_is_sign_invariant(opcode))
#define UOP(op) emit_unary_op(ops[0], ops[1], ops[2], #op)
#define QFOP(op) emit_quaternary_func_op(ops[0], ops[1], ops[2], ops[3], ops[4], ops[5], #op)
#define TFOP(op) emit_trinary_func_op(ops[0], ops[1], ops[2], ops[3], ops[4], #op)
#define BFOP(op) emit_binary_func_op(ops[0], ops[1], ops[2], ops[3], #op)
#define BFOP_CAST(op, type) \
	emit_binary_func_op_cast(ops[0], ops[1], ops[2], ops[3], #op, type, opcode_is_sign_invariant(opcode))
#define BFOP(op) emit_binary_func_op(ops[0], ops[1], ops[2], ops[3], #op)
#define UFOP(op) emit_unary_func_op(ops[0], ops[1], ops[2], #op)

	auto ops = stream(instruction);
	auto opcode = static_cast<Op>(instruction.op);

	switch (opcode)
	{

	// Comparisons
	case OpIEqual:
	case OpLogicalEqual:
	case OpFOrdEqual:
		BOP(==);
		break;

	case OpINotEqual:
	case OpLogicalNotEqual:
	case OpFOrdNotEqual:
		BOP(!=);
		break;

	case OpUGreaterThan:
	case OpSGreaterThan:
	case OpFOrdGreaterThan:
		BOP(>);
		break;

	case OpUGreaterThanEqual:
	case OpSGreaterThanEqual:
	case OpFOrdGreaterThanEqual:
		BOP(>=);
		break;

	case OpULessThan:
	case OpSLessThan:
	case OpFOrdLessThan:
		BOP(<);
		break;

	case OpULessThanEqual:
	case OpSLessThanEqual:
	case OpFOrdLessThanEqual:
		BOP(<=);
		break;

	// Derivatives
	case OpDPdx:
		UFOP(dfdx);
		break;

	case OpDPdy:
		UFOP(dfdy);
		break;

	case OpImageQuerySize:
	{
		auto &type = expression_type(ops[2]);
		uint32_t result_type = ops[0];
		uint32_t id = ops[1];

		if (type.basetype == SPIRType::Image)
		{
			string img_exp = to_expression(ops[2]);
			auto &img_type = type.image;
			switch (img_type.dim)
			{
			case Dim1D:
				if (img_type.arrayed)
					emit_op(result_type, id, join("uint2(", img_exp, ".get_width(), ", img_exp, ".get_array_size())"),
					        false);
				else
					emit_op(result_type, id, join(img_exp, ".get_width()"), true);
				break;

			case Dim2D:
			case DimCube:
				if (img_type.arrayed)
					emit_op(result_type, id, join("uint3(", img_exp, ".get_width(), ", img_exp, ".get_height(), ",
					                              img_exp, ".get_array_size())"),
					        false);
				else
					emit_op(result_type, id, join("uint2(", img_exp, ".get_width(), ", img_exp, ".get_height())"),
					        false);
				break;

			case Dim3D:
				emit_op(result_type, id,
				        join("uint3(", img_exp, ".get_width(), ", img_exp, ".get_height(), ", img_exp, ".get_depth())"),
				        false);
				break;

			default:
				break;
			}
		}
		else
			SPIRV_CROSS_THROW("Invalid type for OpImageQuerySize.");
		break;
	}

	default:
		CompilerGLSL::emit_instruction(instruction);
		break;
	}
}

// Override for MSL-specific extension syntax instructions
void CompilerMSL::emit_glsl_op(uint32_t result_type, uint32_t id, uint32_t eop, const uint32_t *args, uint32_t count)
{
	GLSLstd450 op = static_cast<GLSLstd450>(eop);

	switch (op)
	{
	case GLSLstd450Atan2:
		emit_binary_func_op(result_type, id, args[0], args[1], "atan2");
		break;

	default:
		CompilerGLSL::emit_glsl_op(result_type, id, eop, args, count);
		break;
	}
}

// Emit a structure declaration for the specified interface variable.
void CompilerMSL::emit_interface_block(uint32_t ib_var_id)
{
	if (ib_var_id)
	{
		auto &ib_var = get<SPIRVariable>(ib_var_id);
		auto &ib_type = get<SPIRType>(ib_var.basetype);
		emit_struct(ib_type);
	}
}

// Output a declaration statement for each function.
void CompilerMSL::emit_function_declarations()
{
	for (auto &id : ids)
		if (id.get_type() == TypeFunction)
		{
			auto &func = id.get<SPIRFunction>();
			if (func.self != entry_point)
			{
				auto &dec = meta[func.self].decoration;
				if (dec.alias[0] != 'm')
				{
					// Add prefix to all fuctions in order to avoid ambiguous function names (e.g. builtin functions)
					// TODO: check if current function is a builtin function
					dec.alias = join("m", dec.alias);
				}
				emit_function_prototype(func, true);
			}
		}

	statement("");
}

void CompilerMSL::emit_function_prototype(SPIRFunction &func, uint64_t)
{
	emit_function_prototype(func, false);
}

// Emits the declaration signature of the specified function.
// If this is the entry point function, Metal-specific return value and function arguments are added.
void CompilerMSL::emit_function_prototype(SPIRFunction &func, bool is_decl)
{
	local_variable_names = resource_names;
	string decl;

	processing_entry_point = (func.self == entry_point);

	auto &type = get<SPIRType>(func.return_type);
	decl += func_type_decl(type);
	decl += " ";
	decl += clean_func_name(to_name(func.self));

	decl += "(";

	if (processing_entry_point)
	{
		decl += entry_point_args(!func.arguments.empty());

		// If entry point function has a output interface struct, set its initializer.
		// This is done at this late stage because the initialization expression is
		// cleared after each compilation pass.
		if (stage_out_var_id)
		{
			auto &so_var = get<SPIRVariable>(stage_out_var_id);
			auto &so_type = get<SPIRType>(so_var.basetype);
			set<SPIRExpression>(so_var.initializer, "{}", so_type.self, true);
		}
	}

	for (auto &arg : func.arguments)
	{
		add_local_variable_name(arg.id);

		bool is_uniform_struct = false;
		auto *var = maybe_get<SPIRVariable>(arg.id);
		if (var)
		{
			var->parameter = &arg; // Hold a pointer to the parameter so we can invalidate the readonly field if needed.

			// Check if this arg is one of the synthetic uniform args
			// created to handle uniform access inside the function
			auto &var_type = get<SPIRType>(var->basetype);
			is_uniform_struct =
			    ((var_type.basetype == SPIRType::Struct) &&
			     (var_type.storage == StorageClassUniform || var_type.storage == StorageClassUniformConstant ||
			      var_type.storage == StorageClassPushConstant));
		}

		decl += (is_uniform_struct ? "constant " : "thread ");
		decl += argument_decl(arg);

		// Manufacture automatic sampler arg for SampledImage texture
		auto &arg_type = get<SPIRType>(arg.type);
		if (arg_type.basetype == SPIRType::SampledImage)
			decl += ", thread const sampler& " + to_sampler_expression(arg.id);

		if (&arg != &func.arguments.back())
			decl += ", ";
	}

	decl += ")";
	statement(decl, (is_decl ? ";" : ""));
}

// Returns the texture sampling function string for the specified image and sampling characteristics.
string CompilerMSL::to_function_name(uint32_t img, const SPIRType &, bool is_fetch, bool is_gather, bool, bool, bool,
                                     bool, bool, bool has_dref)
{
	// Texture reference
	string fname = to_expression(img) + ".";

	// Texture function and sampler
	if (is_fetch)
		fname += "read";
	else if (is_gather)
		fname += "gather";
	else
		fname += "sample";

	if (has_dref)
		fname += "_compare";

	return fname;
}

// Returns the function args for a texture sampling function for the specified image and sampling characteristics.
string CompilerMSL::to_function_args(uint32_t img, const SPIRType &imgtype, bool is_fetch, bool, bool is_proj,
                                     uint32_t coord, uint32_t, uint32_t dref, uint32_t grad_x, uint32_t grad_y,
                                     uint32_t lod, uint32_t coffset, uint32_t offset, uint32_t bias, uint32_t comp,
                                     uint32_t, bool *p_forward)
{
	string farg_str = to_sampler_expression(img);

	// Texture coordinates
	bool forward = should_forward(coord);
	auto coord_expr = to_enclosed_expression(coord);
	string tex_coords = coord_expr;
	const char *alt_coord = "";

	switch (imgtype.image.dim)
	{
	case Dim1D:
		tex_coords = coord_expr + ".x";
		remove_duplicate_swizzle(tex_coords);

		alt_coord = ".y";

		break;

	case Dim2D:
		if (msl_config.flip_frag_y)
		{
			string coord_x = coord_expr + ".x";
			remove_duplicate_swizzle(coord_x);
			string coord_y = coord_expr + ".y";
			remove_duplicate_swizzle(coord_y);
			tex_coords = "float2(" + coord_x + ", (1.0 - " + coord_y + "))";
		}
		else
		{
			tex_coords = coord_expr + ".xy";
			remove_duplicate_swizzle(tex_coords);
		}

		alt_coord = ".z";

		break;

	case Dim3D:
	case DimCube:
		if (msl_config.flip_frag_y)
		{
			string coord_x = coord_expr + ".x";
			remove_duplicate_swizzle(coord_x);
			string coord_y = coord_expr + ".y";
			remove_duplicate_swizzle(coord_y);
			string coord_z = coord_expr + ".z";
			remove_duplicate_swizzle(coord_z);
			tex_coords = "float3(" + coord_x + ", (1.0 - " + coord_y + "), " + coord_z + ")";
		}
		else
		{
			tex_coords = coord_expr + ".xyz";
			remove_duplicate_swizzle(tex_coords);
		}

		alt_coord = ".w";

		break;

	default:
		break;
	}

	// Use alt coord for projection or texture array
	if (imgtype.image.arrayed)
		tex_coords += ", " + coord_expr + alt_coord;
	else if (is_proj)
		tex_coords += " / " + coord_expr + alt_coord;

	farg_str += ", ";
	farg_str += tex_coords;

	// Depth compare reference value
	if (dref)
	{
		forward = forward && should_forward(dref);
		farg_str += ", ";
		farg_str += to_expression(dref);
	}

	// LOD Options
	if (bias)
	{
		forward = forward && should_forward(bias);
		farg_str += ", bias(" + to_expression(bias) + ")";
	}

	if (lod)
	{
		forward = forward && should_forward(lod);
		if (is_fetch)
		{
			farg_str += ", " + to_expression(lod);
		}
		else
		{
			farg_str += ", level(" + to_expression(lod) + ")";
		}
	}

	if (grad_x || grad_y)
	{
		forward = forward && should_forward(grad_x);
		forward = forward && should_forward(grad_y);
		string grad_opt;
		switch (imgtype.image.dim)
		{
		case Dim2D:
			grad_opt = "2d";
			break;
		case Dim3D:
			grad_opt = "3d";
			break;
		case DimCube:
			grad_opt = "cube";
			break;
		default:
			grad_opt = "unsupported_gradient_dimension";
			break;
		}
		farg_str += ", gradient" + grad_opt + "(" + to_expression(grad_x) + ", " + to_expression(grad_y) + ")";
	}

	// Add offsets
	string offset_expr;
	if (coffset)
	{
		forward = forward && should_forward(coffset);
		offset_expr = to_expression(coffset);
	}
	else if (offset)
	{
		forward = forward && should_forward(offset);
		offset_expr = to_expression(offset);
	}

	if (!offset_expr.empty())
	{
		switch (imgtype.image.dim)
		{
		case Dim2D:
			if (msl_config.flip_frag_y)
			{
				string coord_x = offset_expr + ".x";
				remove_duplicate_swizzle(coord_x);
				string coord_y = offset_expr + ".y";
				remove_duplicate_swizzle(coord_y);
				offset_expr = "float2(" + coord_x + ", (1.0 - " + coord_y + "))";
			}
			else
			{
				offset_expr = offset_expr + ".xy";
				remove_duplicate_swizzle(offset_expr);
			}

			farg_str += ", " + offset_expr;
			break;

		case Dim3D:
			if (msl_config.flip_frag_y)
			{
				string coord_x = offset_expr + ".x";
				remove_duplicate_swizzle(coord_x);
				string coord_y = offset_expr + ".y";
				remove_duplicate_swizzle(coord_y);
				string coord_z = offset_expr + ".z";
				remove_duplicate_swizzle(coord_z);
				offset_expr = "float3(" + coord_x + ", (1.0 - " + coord_y + "), " + coord_z + ")";
			}
			else
			{
				offset_expr = offset_expr + ".xyz";
				remove_duplicate_swizzle(offset_expr);
			}

			farg_str += ", " + offset_expr;
			break;

		default:
			break;
		}
	}

	if (comp)
	{
		forward = forward && should_forward(comp);
		farg_str += ", " + to_component_argument(comp);
	}

	*p_forward = forward;

	return farg_str;
}

// Returns a string to use in an image sampling function argument.
// The ID must be a scalar constant.
string CompilerMSL::to_component_argument(uint32_t id)
{
	if (ids[id].get_type() != TypeConstant)
	{
		SPIRV_CROSS_THROW("ID " + to_string(id) + " is not an OpConstant.");
		return "component::x";
	}

	uint32_t component_index = get<SPIRConstant>(id).scalar();
	switch (component_index)
	{
	case 0:
		return "component::x";
	case 1:
		return "component::y";
	case 2:
		return "component::z";
	case 3:
		return "component::w";

	default:
		SPIRV_CROSS_THROW("The value (" + to_string(component_index) + ") of OpConstant ID " + to_string(id) +
		                  " is not a valid Component index, which must be one of 0, 1, 2, or 3.");
		return "component::x";
	}
}

// Establish sampled image as expression object and assign the sampler to it.
void CompilerMSL::emit_sampled_image_op(uint32_t result_type, uint32_t result_id, uint32_t image_id, uint32_t samp_id)
{
	set<SPIRExpression>(result_id, to_expression(image_id), result_type, true);
	meta[result_id].sampler = samp_id;
}

// Returns a string representation of the ID, usable as a function arg.
// Manufacture automatic sampler arg for SampledImage texture.
string CompilerMSL::to_func_call_arg(uint32_t id)
{
	string arg_str = CompilerGLSL::to_func_call_arg(id);

	// Manufacture automatic sampler arg if the arg is a SampledImage texture.
	Variant &id_v = ids[id];
	if (id_v.get_type() == TypeVariable)
	{
		auto &var = id_v.get<SPIRVariable>();
		auto &type = get<SPIRType>(var.basetype);
		if (type.basetype == SPIRType::SampledImage)
			arg_str += ", " + to_sampler_expression(id);
	}

	return arg_str;
}

// If the ID represents a sampled image that has been assigned a sampler already,
// generate an expression for the sampler, otherwise generate a fake sampler name
// by appending a suffix to the expression constructed from the ID.
string CompilerMSL::to_sampler_expression(uint32_t id)
{
	uint32_t samp_id = meta[id].sampler;
	return samp_id ? to_expression(samp_id) : to_expression(id) + sampler_name_suffix;
}

// Called automatically at the end of the entry point function
void CompilerMSL::emit_fixup()
{
	auto &execution = get_entry_point();

	if ((execution.model == ExecutionModelVertex) && stage_out_var_id && !qual_pos_var_name.empty())
	{
		if (options.vertex.fixup_clipspace)
		{
			/*const char *suffix = backend.float_literal_suffix ? "f" : "";
			statement(qual_pos_var_name, ".z = 2.0", suffix, " * ", qual_pos_var_name, ".z - ", qual_pos_var_name,
			          ".w;", "    // Adjust clip-space for Metal");*/
			statement(qual_pos_var_name, ".z = (", qual_pos_var_name, ".z + ", qual_pos_var_name,
			          ".w) * 0.5;       // Adjust clip-space for Metal");
		}

		if (msl_config.flip_vert_y)
			statement(qual_pos_var_name, ".y = -(", qual_pos_var_name, ".y);", "    // Invert Y-axis for Metal");
	}
}

// Returns a declaration for a structure member.
string CompilerMSL::member_decl(const SPIRType &type, const SPIRType &membertype, uint32_t index)
{
	return join(type_to_glsl(membertype), " ", to_member_name(type, index), type_to_array_glsl(membertype),
	            member_attribute_qualifier(type, index));
}

// Return a MSL qualifier for the specified function attribute member
string CompilerMSL::member_attribute_qualifier(const SPIRType &type, uint32_t index)
{
	auto &execution = get_entry_point();

	BuiltIn builtin;
	bool is_builtin = is_member_builtin(type, index, &builtin);

	// Vertex function inputs
	if (execution.model == ExecutionModelVertex && type.storage == StorageClassInput)
	{
		if (is_builtin)
		{
			switch (builtin)
			{
			case BuiltInVertexId:
			case BuiltInVertexIndex:
			case BuiltInInstanceId:
			case BuiltInInstanceIndex:
				return string(" [[") + builtin_qualifier(builtin) + "]]";

			default:
				return "";
			}
		}
		uint32_t locn = get_ordered_member_location(type.self, index);
		return string(" [[attribute(") + convert_to_string(locn) + ")]]";
	}

	// Vertex function outputs
	if (execution.model == ExecutionModelVertex && type.storage == StorageClassOutput)
	{
		if (is_builtin)
		{
			switch (builtin)
			{
			case BuiltInClipDistance:
				return " /* [[clip_distance]] built-in not yet supported under Metal. */";

			case BuiltInPointSize: // Must output only if really rendering points
				return msl_config.is_rendering_points ? (string(" [[") + builtin_qualifier(builtin) + "]]") : "";

			case BuiltInPosition:
			case BuiltInLayer:
				return string(" [[") + builtin_qualifier(builtin) + "]]";

			default:
				return "";
			}
		}
		uint32_t locn = get_ordered_member_location(type.self, index);
		return string(" [[user(locn") + convert_to_string(locn) + ")]]";
	}

	// Fragment function inputs
	if (execution.model == ExecutionModelFragment && type.storage == StorageClassInput)
	{
		if (is_builtin)
		{
			switch (builtin)
			{
			case BuiltInFrontFacing:
			case BuiltInPointCoord:
			case BuiltInFragCoord:
			case BuiltInSampleId:
			case BuiltInSampleMask:
			case BuiltInLayer:
				return string(" [[") + builtin_qualifier(builtin) + "]]";

			default:
				return "";
			}
		}
		uint32_t locn = get_ordered_member_location(type.self, index);
		return string(" [[user(locn") + convert_to_string(locn) + ")]]";
	}

	// Fragment function outputs
	if (execution.model == ExecutionModelFragment && type.storage == StorageClassOutput)
	{
		if (is_builtin)
		{
			switch (builtin)
			{
			case BuiltInSampleMask:
			case BuiltInFragDepth:
				return string(" [[") + builtin_qualifier(builtin) + "]]";

			default:
				return "";
			}
		}
		uint32_t locn = get_ordered_member_location(type.self, index);
		return string(" [[color(") + convert_to_string(locn) + ")]]";
	}

	return "";
}

// Returns the location decoration of the member with the specified index in the specified type.
// If the location of the member has been explicitly set, that location is used. If not, this
// function assumes the members are ordered in their location order, and simply returns the
// index as the location.
uint32_t CompilerMSL::get_ordered_member_location(uint32_t type_id, uint32_t index)
{
	auto &m = meta.at(type_id);
	if (index < m.members.size())
	{
		auto &dec = m.members[index];
		if (dec.decoration_flags & (1ull << DecorationLocation))
			return dec.location;
	}

	return index;
}

string CompilerMSL::constant_expression(const SPIRConstant &c)
{
	if (!c.subconstants.empty())
	{
		// Handles Arrays and structures.
		string res = "{";
		for (auto &elem : c.subconstants)
		{
			res += constant_expression(get<SPIRConstant>(elem));
			if (&elem != &c.subconstants.back())
				res += ", ";
		}
		res += "}";
		return res;
	}
	else if (c.columns() == 1)
	{
		return constant_expression_vector(c, 0);
	}
	else
	{
		string res = type_to_glsl(get<SPIRType>(c.constant_type)) + "(";
		for (uint32_t col = 0; col < c.columns(); col++)
		{
			res += constant_expression_vector(c, col);
			if (col + 1 < c.columns())
				res += ", ";
		}
		res += ")";
		return res;
	}
}

// Returns the type declaration for a function, including the
// entry type if the current function is the entry point function
string CompilerMSL::func_type_decl(SPIRType &type)
{
	auto &execution = get_entry_point();
	// The regular function return type. If not processing the entry point function, that's all we need
	string return_type = type_to_glsl(type);
	if (!processing_entry_point)
		return return_type;

	// If an outgoing interface block has been defined, override the entry point return type
	if (stage_out_var_id)
	{
		auto &so_var = get<SPIRVariable>(stage_out_var_id);
		auto &so_type = get<SPIRType>(so_var.basetype);
		return_type = type_to_glsl(so_type);
	}

	// Prepend a entry type, based on the execution model
	string entry_type;
	switch (execution.model)
	{
	case ExecutionModelVertex:
		entry_type = "vertex";
		break;
	case ExecutionModelFragment:
		entry_type = (execution.flags & (1ull << ExecutionModeEarlyFragmentTests)) ?
		                 "fragment [[ early_fragment_tests ]]" :
		                 "fragment";
		break;
	case ExecutionModelGLCompute:
	case ExecutionModelKernel:
		entry_type = "kernel";
		break;
	default:
		entry_type = "unknown";
		break;
	}

	return entry_type + " " + return_type;
}

// Ensures the function name is not "main", which is illegal in MSL
string CompilerMSL::clean_func_name(string func_name)
{
	auto iter = func_name_overrides.find(func_name);
	return (iter != func_name_overrides.end()) ? iter->second : func_name;
}

void CompilerMSL::set_entry_point_name(string func_name)
{
	if (func_name.find("main") == std::string::npos)
		func_name += "_main";
	meta.at(entry_point).decoration.alias = func_name;
}

// Returns a string containing a comma-delimited list of args for the entry point function
string CompilerMSL::entry_point_args(bool append_comma)
{
	string ep_args;

	// Stage-in structure
	if (stage_in_var_id)
	{
		auto &var = get<SPIRVariable>(stage_in_var_id);
		auto &type = get<SPIRType>(var.basetype);

		if (!ep_args.empty())
			ep_args += ", ";

		ep_args += type_to_glsl(type) + " " + to_name(var.self) + " [[stage_in]]";
	}

	// Uniforms
	for (auto &id : ids)
	{
		if (id.get_type() == TypeVariable)
		{
			auto &var = id.get<SPIRVariable>();
			auto &type = get<SPIRType>(var.basetype);

			if ((var.storage == StorageClassUniform || var.storage == StorageClassUniformConstant ||
			     var.storage == StorageClassPushConstant))
			{
				switch (type.basetype)
				{
				case SPIRType::Struct:
					if (!ep_args.empty())
						ep_args += ", ";
					ep_args += "constant " + type_to_glsl(type) + "& " + to_name(var.self);
					ep_args += " [[buffer(" + convert_to_string(get_metal_resource_index(var, type.basetype)) + ")]]";
					break;
				case SPIRType::Sampler:
					if (!ep_args.empty())
						ep_args += ", ";
					ep_args += type_to_glsl(type) + " " + to_name(var.self);
					ep_args += " [[sampler(" + convert_to_string(get_metal_resource_index(var, type.basetype)) + ")]]";
					break;
				case SPIRType::Image:
					if (!ep_args.empty())
						ep_args += ", ";
					ep_args += type_to_glsl(type) + " " + to_name(var.self);
					ep_args += " [[texture(" + convert_to_string(get_metal_resource_index(var, type.basetype)) + ")]]";
					break;
				case SPIRType::SampledImage:
					if (!ep_args.empty())
						ep_args += ", ";
					ep_args += type_to_glsl(type) + " " + to_name(var.self);
					ep_args +=
					    " [[texture(" + convert_to_string(get_metal_resource_index(var, SPIRType::Image)) + ")]]";
					if (type.image.dim != DimBuffer)
					{
						ep_args += ", sampler " + to_sampler_expression(var.self);
						ep_args +=
						    " [[sampler(" + convert_to_string(get_metal_resource_index(var, SPIRType::Sampler)) + ")]]";
					}
					break;
				default:
					break;
				}
			}
			if (var.storage == StorageClassInput && is_builtin_variable(var))
			{
				if (!ep_args.empty())
					ep_args += ", ";
				BuiltIn bi_type = meta[var.self].decoration.builtin_type;
				ep_args += builtin_type_decl(bi_type) + " " + to_expression(var.self);
				ep_args += " [[" + builtin_qualifier(bi_type) + "]]";
			}
		}
	}

	if (!ep_args.empty() && append_comma)
		ep_args += ", ";

	return ep_args;
}

// Returns the Metal index of the resource of the specified type as used by the specified variable.
uint32_t CompilerMSL::get_metal_resource_index(SPIRVariable &var, SPIRType::BaseType basetype)
{
	auto &execution = get_entry_point();
	auto &var_dec = meta[var.self].decoration;
	uint32_t var_desc_set = (var.storage == StorageClassPushConstant) ? kPushConstDescSet : var_dec.set;
	uint32_t var_binding = (var.storage == StorageClassPushConstant) ? kPushConstBinding : var_dec.binding;

	// If a matching binding has been specified, find and use it
	for (auto p_res_bind : resource_bindings)
	{
		if (p_res_bind->stage == execution.model && p_res_bind->desc_set == var_desc_set &&
		    p_res_bind->binding == var_binding)
		{

			p_res_bind->used_by_shader = true;
			switch (basetype)
			{
			case SPIRType::Struct:
				return p_res_bind->msl_buffer;
			case SPIRType::Image:
				return p_res_bind->msl_texture;
			case SPIRType::Sampler:
				return p_res_bind->msl_sampler;
			default:
				return 0;
			}
		}
	}

	// If a binding has not been specified, revert to incrementing resource indices
	switch (basetype)
	{
	case SPIRType::Struct:
		if (execution.model == ExecutionModelVertex && next_metal_resource_index.msl_buffer == 0)
			next_metal_resource_index.msl_buffer = 1;
		return next_metal_resource_index.msl_buffer++;
	case SPIRType::Image:
		return next_metal_resource_index.msl_texture++;
	case SPIRType::Sampler:
		return next_metal_resource_index.msl_sampler++;
	default:
		return 0;
	}
}

// Returns the name of the entry point of this shader
string CompilerMSL::get_entry_point_name()
{
	return clean_func_name(to_name(entry_point));
}

string CompilerMSL::argument_decl(const SPIRFunction::Parameter &arg)
{
	auto &type = expression_type(arg.id);
	bool constref = !type.pointer || arg.write_count == 0;

	// TODO: Check if this arg is an uniform pointer
	bool pointer = type.storage == StorageClassUniformConstant;

	auto &var = get<SPIRVariable>(arg.id);
	return join(constref ? "const " : "", type_to_glsl(type), pointer ? " " : "& ", to_name(var.self),
	            type_to_array_glsl(type));
}

// If we're currently in the entry point function, and the object
// has a qualified name, use it, otherwise use the standard name.
string CompilerMSL::to_name(uint32_t id, bool allow_alias)
{
	if (current_function && (current_function->self == entry_point))
	{
		string qual_name = meta.at(id).decoration.qualified_alias;
		if (!qual_name.empty())
			return qual_name;
	}
	return Compiler::to_name(id, allow_alias);
}

// Returns a name that combines the name of the struct with the name of the member, except for Builtins
string CompilerMSL::to_qualified_member_name(const SPIRType &type, uint32_t index)
{
	// Don't qualify Builtin names because they are unique and are treated as such when building expressions
	BuiltIn builtin;
	if (is_member_builtin(type, index, &builtin))
		return builtin_to_glsl(builtin);

	// Strip any underscore prefix from member name
	string mbr_name = to_member_name(type, index);
	size_t startPos = mbr_name.find_first_not_of("_");
	mbr_name = (startPos != std::string::npos) ? mbr_name.substr(startPos) : "";
	return join(to_name(type.self), "_", mbr_name);
}

// Ensures that the specified name is permanently usable by prepending a prefix
// if the first chars are _ and a digit, which indicate a transient name.
string CompilerMSL::ensure_valid_name(string name, string pfx)
{
	if (name.size() >= 2 && name[0] == '_' && isdigit(name[1]))
	{
		return join(pfx, name);
	}
	else if (std::find(reserved_names.begin(), reserved_names.end(), name) != reserved_names.end())
	{
		return join(pfx, name);
	}
	else
	{
		return name;
	}
}

// Returns an MSL string describing  the SPIR-V type
string CompilerMSL::type_to_glsl(const SPIRType &type)
{
	// Ignore the pointer type since GLSL doesn't have pointers.

	switch (type.basetype)
	{
	case SPIRType::Struct:
		// Need OpName lookup here to get a "sensible" name for a struct.
		return to_name(type.self);

	case SPIRType::Image:
	case SPIRType::SampledImage:
		return image_type_glsl(type);

	case SPIRType::Sampler:
		// Not really used.
		return "sampler";

	case SPIRType::Void:
		return "void";

	default:
		break;
	}

	if (is_scalar(type)) // Scalar builtin
	{
		switch (type.basetype)
		{
		case SPIRType::Boolean:
			return "bool";
		case SPIRType::Char:
			return "char";
		case SPIRType::Int:
			return (type.width == 16 ? "short" : "int");
		case SPIRType::UInt:
			return (type.width == 16 ? "ushort" : "uint");
		case SPIRType::AtomicCounter:
			return "atomic_uint";
		case SPIRType::Float:
			return (type.width == 16 ? "half" : "float");
		default:
			return "unknown_type";
		}
	}
	else if (is_vector(type)) // Vector builtin
	{
		switch (type.basetype)
		{
		case SPIRType::Boolean:
			return join("bool", type.vecsize);
		case SPIRType::Char:
			return join("char", type.vecsize);
			;
		case SPIRType::Int:
			return join((type.width == 16 ? "short" : "int"), type.vecsize);
		case SPIRType::UInt:
			return join((type.width == 16 ? "ushort" : "uint"), type.vecsize);
		case SPIRType::Float:
			return join((type.width == 16 ? "half" : "float"), type.vecsize);
		default:
			return "unknown_type";
		}
	}
	else
	{
		switch (type.basetype)
		{
		case SPIRType::Boolean:
		case SPIRType::Int:
		case SPIRType::UInt:
		case SPIRType::Float:
			return join((type.width == 16 ? "half" : "float"), type.columns, "x", type.vecsize);
		default:
			return "unknown_type";
		}
	}
}

// Returns an MSL string describing  the SPIR-V image type
string CompilerMSL::image_type_glsl(const SPIRType &type)
{
	string img_type_name;

	auto &img_type = type.image;
	if (img_type.depth)
	{
		switch (img_type.dim)
		{
		case spv::Dim2D:
			img_type_name += (img_type.ms ? "depth2d_ms" : (img_type.arrayed ? "depth2d_array" : "depth2d"));
			break;
		case spv::DimCube:
			img_type_name += (img_type.arrayed ? "depthcube_array" : "depthcube");
			break;
		default:
			img_type_name += "unknown_depth_texture_type";
			break;
		}
	}
	else
	{
		switch (img_type.dim)
		{
		case spv::Dim1D:
			img_type_name += (img_type.arrayed ? "texture1d_array" : "texture1d");
			break;
		case spv::DimBuffer:
		case spv::Dim2D:
			img_type_name += (img_type.ms ? "texture2d_ms" : (img_type.arrayed ? "texture2d_array" : "texture2d"));
			break;
		case spv::Dim3D:
			img_type_name += "texture3d";
			break;
		case spv::DimCube:
			img_type_name += (img_type.arrayed ? "texturecube_array" : "texturecube");
			break;
		default:
			img_type_name += "unknown_texture_type";
			break;
		}
	}

	// Append the pixel type
	auto &img_pix_type = get<SPIRType>(img_type.type);
	img_type_name += "<" + type_to_glsl(img_pix_type) + ">";

	return img_type_name;
}

// Returns an MSL string identifying the name of a SPIR-V builtin.
// Output builtins are qualified with the name of the stage out structure.
string CompilerMSL::builtin_to_glsl(BuiltIn builtin)
{
	switch (builtin)
	{

	// Override GLSL compiler strictness
	case BuiltInVertexId:
		return "gl_VertexID";
	case BuiltInInstanceId:
		return "gl_InstanceID";
	case BuiltInVertexIndex:
		return "gl_VertexIndex";
	case BuiltInInstanceIndex:
		return "gl_InstanceIndex";

	// Output builtins qualified with output struct when used in the entry function
	case BuiltInPosition:
	case BuiltInPointSize:
	case BuiltInClipDistance:
	case BuiltInLayer:
		if (current_function && (current_function->self == entry_point))
			return stage_out_var_name + "." + CompilerGLSL::builtin_to_glsl(builtin);
		else
			return CompilerGLSL::builtin_to_glsl(builtin);

	default:
		return CompilerGLSL::builtin_to_glsl(builtin);
	}
}

// Returns an MSL string attribute qualifer for a SPIR-V builtin
string CompilerMSL::builtin_qualifier(BuiltIn builtin)
{
	auto &execution = get_entry_point();

	switch (builtin)
	{
	// Vertex function in
	case BuiltInVertexId:
		return "vertex_id";
	case BuiltInVertexIndex:
		return "vertex_id";
	case BuiltInInstanceId:
		return "instance_id";
	case BuiltInInstanceIndex:
		return "instance_id";

	// Vertex function out
	case BuiltInClipDistance:
		return "clip_distance";
	case BuiltInPointSize:
		return "point_size";
	case BuiltInPosition:
		return "position";
	case BuiltInLayer:
		return "render_target_array_index";

	// Fragment function in
	case BuiltInFrontFacing:
		return "front_facing";
	case BuiltInPointCoord:
		return "point_coord";
	case BuiltInFragCoord:
		return "position";
	case BuiltInSampleId:
		return "sample_id";
	case BuiltInSampleMask:
		return "sample_mask";

	// Fragment function out
	case BuiltInFragDepth:
	{
		if (execution.flags & (1ull << ExecutionModeDepthGreater))
			return "depth(greater)";

		if (execution.flags & (1ull << ExecutionModeDepthLess))
			return "depth(less)";

		if (execution.flags & (1ull << ExecutionModeDepthUnchanged))
			return "depth(any)";
	}

	default:
		return "unsupported-built-in";
	}
}

// Returns an MSL string type declaration for a SPIR-V builtin
string CompilerMSL::builtin_type_decl(BuiltIn builtin)
{
	switch (builtin)
	{
	// Vertex function in
	case BuiltInVertexId:
		return "uint";
	case BuiltInVertexIndex:
		return "uint";
	case BuiltInInstanceId:
		return "uint";
	case BuiltInInstanceIndex:
		return "uint";

	// Vertex function out
	case BuiltInClipDistance:
		return "float";
	case BuiltInPointSize:
		return "float";
	case BuiltInPosition:
		return "float4";

	// Fragment function in
	case BuiltInFrontFacing:
		return "bool";
	case BuiltInPointCoord:
		return "float2";
	case BuiltInFragCoord:
		return "float4";
	case BuiltInSampleId:
		return "uint";
	case BuiltInSampleMask:
		return "uint";

	default:
		return "unsupported-built-in-type";
	}
}

// Returns the effective size of a buffer block struct member.
size_t CompilerMSL::get_declared_struct_member_size(const SPIRType &struct_type, uint32_t index) const
{
	uint32_t type_id = struct_type.member_types[index];
	auto dec_mask = get_member_decoration_mask(struct_type.self, index);
	return get_declared_type_size(type_id, dec_mask);
}

// Returns the effective size of a variable type.
size_t CompilerMSL::get_declared_type_size(uint32_t type_id) const
{
	return get_declared_type_size(type_id, get_decoration_mask(type_id));
}

// Returns the effective size of a variable type or member type,
// taking into consideration the specified mask of decorations.
size_t CompilerMSL::get_declared_type_size(uint32_t type_id, uint64_t dec_mask) const
{
	auto &type = get<SPIRType>(type_id);

	if (type.basetype == SPIRType::Struct)
		return get_declared_struct_size(type);

	switch (type.basetype)
	{
	case SPIRType::Unknown:
	case SPIRType::Void:
	case SPIRType::AtomicCounter:
	case SPIRType::Image:
	case SPIRType::SampledImage:
	case SPIRType::Sampler:
		SPIRV_CROSS_THROW("Querying size of object with opaque size.");
	default:
		break;
	}

	size_t component_size = type.width / 8;
	unsigned vecsize = type.vecsize;
	unsigned columns = type.columns;

	if (!type.array.empty())
	{
		// For arrays, we can use ArrayStride to get an easy check if it has been populated.
		// ArrayStride is part of the array type not OpMemberDecorate.
		auto &dec = meta[type_id].decoration;
		if (dec.decoration_flags & (1ull << DecorationArrayStride))
			return dec.array_stride * to_array_size_literal(type, uint32_t(type.array.size()) - 1);
	}

	// Vectors.
	if (columns == 1)
		return vecsize * component_size;
	else
	{
		// Per SPIR-V spec, matrices must be tightly packed and aligned up for vec3 accesses.
		if ((dec_mask & (1ull << DecorationRowMajor)) && columns == 3)
			columns = 4;
		else if ((dec_mask & (1ull << DecorationColMajor)) && vecsize == 3)
			vecsize = 4;

		return vecsize * columns * component_size;
	}
}

// If the opcode requires a bespoke custom function be output, remember it.
bool CompilerMSL::CustomFunctionHandler::handle(Op opcode, const uint32_t * /*args*/, uint32_t /*length*/)
{
	switch (opcode)
	{
	case OpFMod:
		custom_function_ops.insert(opcode);
		break;

	default:
		break;
	}
	return true;
}

// Sort both type and meta member content based on builtin status (put builtins at end),
// then by the required sorting aspect.
void CompilerMSL::MemberSorter::sort()
{
	// Create a temporary array of consecutive member indices and sort it base on how
	// the members should be reordered, based on builtin and sorting aspect meta info.
	size_t mbr_cnt = type.member_types.size();
	vector<uint32_t> mbr_idxs(mbr_cnt);
	iota(mbr_idxs.begin(), mbr_idxs.end(), 0); // Fill with consecutive indices
	std::sort(mbr_idxs.begin(), mbr_idxs.end(), *this); // Sort member indices based on sorting aspect

	// Move type and meta member info to the order defined by the sorted member indices.
	// This is done by creating temporary copies of both member types and meta, and then
	// copying back to the original content at the sorted indices.
	auto mbr_types_cpy = type.member_types;
	auto mbr_meta_cpy = meta.members;
	for (uint32_t mbr_idx = 0; mbr_idx < mbr_cnt; mbr_idx++)
	{
		type.member_types[mbr_idx] = mbr_types_cpy[mbr_idxs[mbr_idx]];
		meta.members[mbr_idx] = mbr_meta_cpy[mbr_idxs[mbr_idx]];
	}
}

// Sort first by builtin status (put builtins at end), then by the sorting aspect.
bool CompilerMSL::MemberSorter::operator()(uint32_t mbr_idx1, uint32_t mbr_idx2)
{
	auto &mbr_meta1 = meta.members[mbr_idx1];
	auto &mbr_meta2 = meta.members[mbr_idx2];
	if (mbr_meta1.builtin != mbr_meta2.builtin)
		return mbr_meta2.builtin;
	else
		switch (sort_aspect)
		{
		case Location:
			return mbr_meta1.location < mbr_meta2.location;
		case LocationReverse:
			return mbr_meta1.location > mbr_meta2.location;
		case Offset:
			return mbr_meta1.offset < mbr_meta2.offset;

		case OffsetThenLocationReverse:
			return (mbr_meta1.offset < mbr_meta2.offset) ||
			       ((mbr_meta1.offset == mbr_meta2.offset) && (mbr_meta1.location > mbr_meta2.location));
		case Alphabetical:
			return mbr_meta1.alias > mbr_meta2.alias;
		default:
			return false;
		}
}
