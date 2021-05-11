/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
/**
 * Yul dialects for EVM.
 */

#include <libyul/backends/evm/EVMDialect.h>

#include <libyul/AsmAnalysisInfo.h>
#include <libyul/AST.h>
#include <libyul/Object.h>
#include <libyul/Exceptions.h>
#include <libyul/AsmParser.h>
#include <libyul/backends/evm/AbstractAssembly.h>

#include <libevmasm/SemanticInformation.h>
#include <libevmasm/Instruction.h>

#include <liblangutil/Exceptions.h>

#include <range/v3/view/reverse.hpp>

using namespace std;
using namespace solidity;
using namespace solidity::yul;
using namespace solidity::util;

namespace
{

void visitArguments(
	AbstractAssembly& _assembly,
	FunctionCall const& _call,
	function<void(Expression const&)> _visitExpression
)
{
	for (auto const& arg: _call.arguments | ranges::views::reverse)
		_visitExpression(arg);

	_assembly.setSourceLocation(_call.location);
}


pair<YulString, BuiltinFunctionForEVM> createEVMFunction(
	string const& _name,
	evmasm::Instruction _instruction
)
{
	evmasm::InstructionInfo info = evmasm::instructionInfo(_instruction);
	BuiltinFunctionForEVM f;
	f.name = YulString{_name};
	f.parameters.resize(static_cast<size_t>(info.args));
	f.returns.resize(static_cast<size_t>(info.ret));
	f.sideEffects = EVMDialect::sideEffectsOfInstruction(_instruction);
	f.controlFlowSideEffects.terminates = evmasm::SemanticInformation::terminatesControlFlow(_instruction);
	f.controlFlowSideEffects.reverts = evmasm::SemanticInformation::reverts(_instruction);
	f.isMSize = _instruction == evmasm::Instruction::MSIZE;
	f.literalArguments.clear();
	f.instruction = _instruction;
	f.generateCode = [_instruction](
		FunctionCall const& _call,
		AbstractAssembly& _assembly,
		BuiltinContext&,
		std::function<void(Expression const&)> _visitExpression
	) {
		visitArguments(_assembly, _call, _visitExpression);
		_assembly.appendInstruction(_instruction);
	};

	return {f.name, move(f)};
}

pair<YulString, BuiltinFunctionForEVM> createFunction(
	string _name,
	size_t _params,
	size_t _returns,
	SideEffects _sideEffects,
	vector<optional<LiteralKind>> _literalArguments,
	std::function<void(FunctionCall const&, AbstractAssembly&, BuiltinContext&, std::function<void(Expression const&)>)> _generateCode
)
{
	yulAssert(_literalArguments.size() == _params || _literalArguments.empty(), "");

	YulString name{std::move(_name)};
	BuiltinFunctionForEVM f;
	f.name = name;
	f.parameters.resize(_params);
	f.returns.resize(_returns);
	f.sideEffects = std::move(_sideEffects);
	f.literalArguments = std::move(_literalArguments);
	f.isMSize = false;
	f.instruction = {};
	f.generateCode = std::move(_generateCode);
	return {name, f};
}

set<YulString> createReservedIdentifiers()
{
	set<YulString> reserved;
	for (auto const& instr: evmasm::c_instructions)
	{
		string name = instr.first;
		transform(name.begin(), name.end(), name.begin(), [](unsigned char _c) { return tolower(_c); });
		reserved.emplace(name);
	}
	reserved += vector<YulString>{
		"linkersymbol"_yulstring,
		"datasize"_yulstring,
		"dataoffset"_yulstring,
		"datacopy"_yulstring,
		"setimmutable"_yulstring,
		"loadimmutable"_yulstring,
	};
	return reserved;
}

map<YulString, BuiltinFunctionForEVM> createBuiltins(langutil::EVMVersion _evmVersion, bool _objectAccess)
{
	map<YulString, BuiltinFunctionForEVM> builtins;
	for (auto const& instr: evmasm::c_instructions)
	{
		string name = instr.first;
		transform(name.begin(), name.end(), name.begin(), [](unsigned char _c) { return tolower(_c); });
		auto const opcode = instr.second;

		if (
			!evmasm::isDupInstruction(opcode) &&
			!evmasm::isSwapInstruction(opcode) &&
			!evmasm::isPushInstruction(opcode) &&
			opcode != evmasm::Instruction::JUMP &&
			opcode != evmasm::Instruction::JUMPI &&
			opcode != evmasm::Instruction::JUMPDEST &&
			_evmVersion.hasOpcode(opcode)
		)
			builtins.emplace(createEVMFunction(name, opcode));
	}

	// OVM changes: "kall", the safe execution manager call.  This function is created
	// as a builtin which can be accessed via inline assembly, or internally to the compiler
	// as done in CompilerContext.cpp
	// NOTE: the opcodes below DO NOT MATCH the SafetyChecker.sol.  This is intentional; we
	// use some different opcodes (of the same total length) here so that the solidity optimizer
	// plays nice with it, and we replace with the right string in CompilerStack.cpp.
	builtins.emplace(createFunction(
		"kall",
		4,
		0,
		SideEffects{false, false, false, false, true},
		{},
		[](
			FunctionCall const& _call,
			AbstractAssembly& _assembly,
			BuiltinContext&,
			std::function<void(Expression const&)> _visitExpression
		) {
			visitArguments(_assembly, _call, _visitExpression);

			_assembly.appendInstruction(evmasm::Instruction::OVM_PLACEHOLDER_CALLER);
			_assembly.appendConstant(0);
			_assembly.appendInstruction(evmasm::Instruction::SWAP1);
			_assembly.appendInstruction(evmasm::Instruction::GAS);
			_assembly.appendInstruction(evmasm::Instruction::OVM_PLACEHOLDER_CALL);
			_assembly.appendInstruction(evmasm::Instruction::PC);
			_assembly.appendConstant(29);
			_assembly.appendInstruction(evmasm::Instruction::ADD);
			_assembly.appendInstruction(evmasm::Instruction::JUMPI);

			_assembly.appendInstruction(evmasm::Instruction::RETURNDATASIZE);
			_assembly.appendConstant(1);
			_assembly.appendInstruction(evmasm::Instruction::EQ);
			_assembly.appendInstruction(evmasm::Instruction::PC);
			_assembly.appendConstant(12);
			_assembly.appendInstruction(evmasm::Instruction::ADD);

			_assembly.appendInstruction(evmasm::Instruction::JUMPI);
			_assembly.appendInstruction(evmasm::Instruction::RETURNDATASIZE);
			_assembly.appendConstant(0);
			_assembly.appendInstruction(evmasm::Instruction::DUP1);
			_assembly.appendInstruction(evmasm::Instruction::RETURNDATACOPY);
			_assembly.appendInstruction(evmasm::Instruction::RETURNDATASIZE);

			// begin: changed ops from what we "really want".  Larger pushed values make sure the total bytes are equivalent while avoiding having jumpdests etc.
			_assembly.appendConstant(1193046); // 0x123456, this should be PUSH1 0 in final form but accounts for the two missing jumpdests
			_assembly.appendInstruction(evmasm::Instruction::MSTORE); // instead of REVERT
			_assembly.appendConstant(234); // in place of 1 because optimizer likes duping 1
			_assembly.appendConstant(4252); // in place of 0 because optimizer likes duping 0
			_assembly.appendInstruction(evmasm::Instruction::MSTORE); // instead of RETURN
		}
	));

	// OVM changes: safe identity precompile call
	builtins.emplace(createFunction(
		"kopy",
		4,
		0,
		SideEffects{false, false, false, false, true},
		{},
		[](
			FunctionCall const& _call,
			AbstractAssembly& _assembly,
			BuiltinContext&,
			std::function<void(Expression const&)> _visitExpression
		) {
			visitArguments(_assembly, _call, _visitExpression);
			_assembly.appendInstruction(evmasm::Instruction::CALLER);
			_assembly.appendInstruction(evmasm::Instruction::POP);
			_assembly.appendConstant(0);
			_assembly.appendConstant(4);
			_assembly.appendInstruction(evmasm::Instruction::GAS);
			_assembly.appendInstruction(evmasm::Instruction::CALL);
			_assembly.appendInstruction(evmasm::Instruction::POP);
		}
	));

	if (_objectAccess)
	{
		builtins.emplace(createFunction("linkersymbol", 1, 1, SideEffects{}, {LiteralKind::String}, [](
			FunctionCall const& _call,
			AbstractAssembly& _assembly,
			BuiltinContext&,
			function<void(Expression const&)>
		) {
			yulAssert(_call.arguments.size() == 1, "");
			Expression const& arg = _call.arguments.front();
			_assembly.appendLinkerSymbol(std::get<Literal>(arg).value.str());
		}));

		builtins.emplace(createFunction(
			"memoryguard",
			1,
			1,
			SideEffects{},
			{LiteralKind::Number},
			[](
				FunctionCall const& _call,
				AbstractAssembly& _assembly,
				BuiltinContext&,
				function<void(Expression const&)> _visitExpression
			) {
				visitArguments(_assembly, _call, _visitExpression);
			})
		);

		builtins.emplace(createFunction("datasize", 1, 1, SideEffects{}, {LiteralKind::String}, [](
			FunctionCall const& _call,
			AbstractAssembly& _assembly,
			BuiltinContext& _context,
			std::function<void(Expression const&)> const&
		) {
			yulAssert(_context.currentObject, "No object available.");
			yulAssert(_call.arguments.size() == 1, "");
			Expression const& arg = _call.arguments.front();
			YulString dataName = std::get<Literal>(arg).value;
			if (_context.currentObject->name == dataName)
				_assembly.appendAssemblySize();
			else
			{
				vector<size_t> subIdPath =
					_context.subIDs.count(dataName) == 0 ?
						_context.currentObject->pathToSubObject(dataName) :
						vector<size_t>{_context.subIDs.at(dataName)};
				yulAssert(!subIdPath.empty(), "Could not find assembly object <" + dataName.str() + ">.");
				_assembly.appendDataSize(subIdPath);
			}
		}));
		builtins.emplace(createFunction("dataoffset", 1, 1, SideEffects{}, {LiteralKind::String}, [](
			FunctionCall const& _call,
			AbstractAssembly& _assembly,
			BuiltinContext& _context,
			std::function<void(Expression const&)> const&
		) {
			yulAssert(_context.currentObject, "No object available.");
			yulAssert(_call.arguments.size() == 1, "");
			Expression const& arg = _call.arguments.front();
			YulString dataName = std::get<Literal>(arg).value;
			if (_context.currentObject->name == dataName)
				_assembly.appendConstant(0);
			else
			{
				vector<size_t> subIdPath =
					_context.subIDs.count(dataName) == 0 ?
						_context.currentObject->pathToSubObject(dataName) :
						vector<size_t>{_context.subIDs.at(dataName)};
				yulAssert(!subIdPath.empty(), "Could not find assembly object <" + dataName.str() + ">.");
				_assembly.appendDataOffset(subIdPath);
			}
		}));
		builtins.emplace(createFunction(
			"datacopy",
			3,
			0,
			SideEffects{false, true, false, false, true, SideEffects::None, SideEffects::None, SideEffects::Write},
			{},
			[](
				FunctionCall const& _call,
				AbstractAssembly& _assembly,
				BuiltinContext&,
				std::function<void(Expression const&)> _visitExpression
			) {
				visitArguments(_assembly, _call, _visitExpression);
				_assembly.appendInstruction(evmasm::Instruction::CODECOPY);
			}
		));
		builtins.emplace(createFunction(
			"setimmutable",
			3,
			0,
			SideEffects{false, false, false, false, true, SideEffects::None, SideEffects::None, SideEffects::Write},
			{std::nullopt, LiteralKind::String, std::nullopt},
			[](
				FunctionCall const& _call,
				AbstractAssembly& _assembly,
				BuiltinContext&,
				std::function<void(Expression const&)> _visitExpression
			) {
				yulAssert(_call.arguments.size() == 3, "");

				_visitExpression(_call.arguments[2]);
				YulString identifier = std::get<Literal>(_call.arguments[1]).value;
				_visitExpression(_call.arguments[0]);
				_assembly.setSourceLocation(_call.location);
				_assembly.appendImmutableAssignment(identifier.str());
			}
		));
		builtins.emplace(createFunction(
			"loadimmutable",
			1,
			1,
			SideEffects{},
			{LiteralKind::String},
			[](
				FunctionCall const& _call,
				AbstractAssembly& _assembly,
				BuiltinContext&,
				std::function<void(Expression const&)>
			) {
				yulAssert(_call.arguments.size() == 1, "");
				_assembly.appendImmutable(std::get<Literal>(_call.arguments.front()).value.str());
			}
		));
	}
	return builtins;
}

}


EVMDialect::EVMDialect(langutil::EVMVersion _evmVersion, bool _objectAccess):
	m_objectAccess(_objectAccess),
	m_evmVersion(_evmVersion),
	m_functions(createBuiltins(_evmVersion, _objectAccess)),
	m_reserved(createReservedIdentifiers())
{
}

BuiltinFunctionForEVM const* EVMDialect::builtin(YulString _name) const
{
	auto it = m_functions.find(_name);
	if (it != m_functions.end())
		return &it->second;
	else
		return nullptr;
}

bool EVMDialect::reservedIdentifier(YulString _name) const
{
	return m_reserved.count(_name) != 0;
}

EVMDialect const& EVMDialect::strictAssemblyForEVM(langutil::EVMVersion _version)
{
	static map<langutil::EVMVersion, unique_ptr<EVMDialect const>> dialects;
	static YulStringRepository::ResetCallback callback{[&] { dialects.clear(); }};
	if (!dialects[_version])
		dialects[_version] = make_unique<EVMDialect>(_version, false);
	return *dialects[_version];
}

EVMDialect const& EVMDialect::strictAssemblyForEVMObjects(langutil::EVMVersion _version)
{
	static map<langutil::EVMVersion, unique_ptr<EVMDialect const>> dialects;
	static YulStringRepository::ResetCallback callback{[&] { dialects.clear(); }};
	if (!dialects[_version])
		dialects[_version] = make_unique<EVMDialect>(_version, true);
	return *dialects[_version];
}

SideEffects EVMDialect::sideEffectsOfInstruction(evmasm::Instruction _instruction)
{
	auto translate = [](evmasm::SemanticInformation::Effect _e) -> SideEffects::Effect
	{
		return static_cast<SideEffects::Effect>(_e);
	};

	return SideEffects{
		evmasm::SemanticInformation::movable(_instruction),
		evmasm::SemanticInformation::movableApartFromEffects(_instruction),
		evmasm::SemanticInformation::canBeRemoved(_instruction),
		evmasm::SemanticInformation::canBeRemovedIfNoMSize(_instruction),
		true, // cannotLoop
		translate(evmasm::SemanticInformation::otherState(_instruction)),
		translate(evmasm::SemanticInformation::storage(_instruction)),
		translate(evmasm::SemanticInformation::memory(_instruction)),
	};
}

EVMDialectTyped::EVMDialectTyped(langutil::EVMVersion _evmVersion, bool _objectAccess):
	EVMDialect(_evmVersion, _objectAccess)
{
	defaultType = "u256"_yulstring;
	boolType = "bool"_yulstring;
	types = {defaultType, boolType};

	// Set all types to ``defaultType``
	for (auto& fun: m_functions)
	{
		for (auto& p: fun.second.parameters)
			p = defaultType;
		for (auto& r: fun.second.returns)
			r = defaultType;
	}

	m_functions["lt"_yulstring].returns = {"bool"_yulstring};
	m_functions["gt"_yulstring].returns = {"bool"_yulstring};
	m_functions["slt"_yulstring].returns = {"bool"_yulstring};
	m_functions["sgt"_yulstring].returns = {"bool"_yulstring};
	m_functions["eq"_yulstring].returns = {"bool"_yulstring};

	// "not" and "bitnot" replace "iszero" and "not"
	m_functions["bitnot"_yulstring] = m_functions["not"_yulstring];
	m_functions["bitnot"_yulstring].name = "bitnot"_yulstring;
	m_functions["not"_yulstring] = m_functions["iszero"_yulstring];
	m_functions["not"_yulstring].name = "not"_yulstring;
	m_functions["not"_yulstring].returns = {"bool"_yulstring};
	m_functions["not"_yulstring].parameters = {"bool"_yulstring};
	m_functions.erase("iszero"_yulstring);

	m_functions["bitand"_yulstring] = m_functions["and"_yulstring];
	m_functions["bitand"_yulstring].name = "bitand"_yulstring;
	m_functions["bitor"_yulstring] = m_functions["or"_yulstring];
	m_functions["bitor"_yulstring].name = "bitor"_yulstring;
	m_functions["bitxor"_yulstring] = m_functions["xor"_yulstring];
	m_functions["bitxor"_yulstring].name = "bitxor"_yulstring;
	m_functions["and"_yulstring].parameters = {"bool"_yulstring, "bool"_yulstring};
	m_functions["and"_yulstring].returns = {"bool"_yulstring};
	m_functions["or"_yulstring].parameters = {"bool"_yulstring, "bool"_yulstring};
	m_functions["or"_yulstring].returns = {"bool"_yulstring};
	m_functions["xor"_yulstring].parameters = {"bool"_yulstring, "bool"_yulstring};
	m_functions["xor"_yulstring].returns = {"bool"_yulstring};
	m_functions["popbool"_yulstring] = m_functions["pop"_yulstring];
	m_functions["popbool"_yulstring].name = "popbool"_yulstring;
	m_functions["popbool"_yulstring].parameters = {"bool"_yulstring};
	m_functions.insert(createFunction("bool_to_u256", 1, 1, {}, {}, [](
		FunctionCall const& _call,
		AbstractAssembly& _assembly,
		BuiltinContext&,
		std::function<void(Expression const&)> _visitExpression
	) {
		visitArguments(_assembly, _call, _visitExpression);
	}));
	m_functions["bool_to_u256"_yulstring].parameters = {"bool"_yulstring};
	m_functions["bool_to_u256"_yulstring].returns = {"u256"_yulstring};
	m_functions.insert(createFunction("u256_to_bool", 1, 1, {}, {}, [](
		FunctionCall const& _call,
		AbstractAssembly& _assembly,
		BuiltinContext&,
		std::function<void(Expression const&)> _visitExpression
	) {
		// TODO this should use a Panic.
		// A value larger than 1 causes an invalid instruction.
		visitArguments(_assembly, _call, _visitExpression);
		_assembly.appendConstant(2);
		_assembly.appendInstruction(evmasm::Instruction::DUP2);
		_assembly.appendInstruction(evmasm::Instruction::LT);
		AbstractAssembly::LabelID inRange = _assembly.newLabelId();
		_assembly.appendJumpToIf(inRange);
		_assembly.appendInstruction(evmasm::Instruction::INVALID);
		_assembly.appendLabel(inRange);
	}));
	m_functions["u256_to_bool"_yulstring].parameters = {"u256"_yulstring};
	m_functions["u256_to_bool"_yulstring].returns = {"bool"_yulstring};
}

BuiltinFunctionForEVM const* EVMDialectTyped::discardFunction(YulString _type) const
{
	if (_type == "bool"_yulstring)
		return builtin("popbool"_yulstring);
	else
	{
		yulAssert(_type == defaultType, "");
		return builtin("pop"_yulstring);
	}
}

BuiltinFunctionForEVM const* EVMDialectTyped::equalityFunction(YulString _type) const
{
	if (_type == "bool"_yulstring)
		return nullptr;
	else
	{
		yulAssert(_type == defaultType, "");
		return builtin("eq"_yulstring);
	}
}

EVMDialectTyped const& EVMDialectTyped::instance(langutil::EVMVersion _version)
{
	static map<langutil::EVMVersion, unique_ptr<EVMDialectTyped const>> dialects;
	static YulStringRepository::ResetCallback callback{[&] { dialects.clear(); }};
	if (!dialects[_version])
		dialects[_version] = make_unique<EVMDialectTyped>(_version, true);
	return *dialects[_version];
}
