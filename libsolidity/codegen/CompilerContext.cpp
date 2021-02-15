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
 * @author Christian <c@ethdev.com>
 * @date 2014
 * Utilities for the solidity compiler.
 */

#include <libsolidity/codegen/CompilerContext.h>

#include <libsolidity/ast/AST.h>
#include <libsolidity/codegen/Compiler.h>
#include <libsolidity/codegen/CompilerUtils.h>
#include <libsolidity/interface/Version.h>

#include <libyul/AsmParser.h>
#include <libyul/AsmAnalysis.h>
#include <libyul/AsmAnalysisInfo.h>
#include <libyul/backends/evm/AsmCodeGen.h>
#include <libyul/backends/evm/EVMDialect.h>
#include <libyul/backends/evm/EVMMetrics.h>
#include <libyul/optimiser/Suite.h>
#include <libyul/Object.h>
#include <libyul/YulString.h>

#include <libsolutil/Whiskers.h>

#include <liblangutil/ErrorReporter.h>
#include <liblangutil/Scanner.h>
#include <liblangutil/SourceReferenceFormatter.h>

#include <boost/algorithm/string/replace.hpp>

#include <utility>
#include <numeric>

// Change to "define" to output all intermediate code
#undef SOL_OUTPUT_ASM
#ifdef SOL_OUTPUT_ASM
#include <libyul/AsmPrinter.h>
#endif


using namespace std;
using namespace solidity;
using namespace solidity::util;
using namespace solidity::evmasm;
using namespace solidity::frontend;
using namespace solidity::langutil;

void CompilerContext::complexRewrite(string function, int _in, int _out,
	string code, vector<string> const& _localVariables, bool optimize=true) {

	auto methodId = FixedHash<4>(util::keccak256(function)).hex();

	auto asm_code = Whiskers(R"({
		let methodId := 0x<methodId>
		// needed to fix synthetix
		let callBytes := msize()
		// replace the first 4 bytes with the right methodID
		mstore(callBytes, shl(224, methodId))
	)")("methodId", methodId).render();

	//cerr << "rewriting " << function << endl;

	for (int i = 0; i < _out-_in; i++) {
		// add padding to the stack, the value doesn't matter
		assemblyPtr()->append(Instruction::GAS);
	}

	if (optimize) {
		callLowLevelFunction(function, 0, 0,
			[asm_code, code, _localVariables](CompilerContext& _context) {
				vector<string> lv = _localVariables;
				lv.push_back("ret");
				_context.m_disable_rewrite = true;
				_context.appendInlineAssembly(asm_code+code, lv);
				_context.m_disable_rewrite = false;
			}
		);
	} else {
		appendInlineAssembly(asm_code+code, _localVariables);
	}

	for (int i = 0; i < _in-_out; i++) {
		assemblyPtr()->append(Instruction::POP);
	}
}

void CompilerContext::simpleRewrite(string function, int _in, int _out, bool optimize=true) {
	assert(_in <= 2);
	assert(_out <= 1);

	auto asm_code = Whiskers(R"(
		<input1>
		<input2>

		// overwrite call params
		kall(callBytes, <in_size>, callBytes, <out_size>)

		<output>

		// overwrite the memory we used back to zero so that it does not mess with downstream use of memory (e.g. bytes memory)
		// need to make larger than 0x40 if we ever use this for inputs exceeding 32*2 bytes in length
		for { let ptr := 0 } lt(ptr, 0x40) { ptr := add(ptr, 0x20) } {
			mstore(add(callBytes, ptr), 0)
		}
	})");
	asm_code("in_size", to_string(_in*0x20+4));
	asm_code("out_size", to_string(_out*0x20));

	asm_code("input1", (_in >= 1) ? "mstore(add(callBytes, 4), x1)" : "");
	asm_code("input2", (_in >= 2) ? "mstore(add(callBytes, 0x24), x2)" : "");
	asm_code("output", (_out > 0) ? "x1 := mload(callBytes)" : "");

	complexRewrite(function, _in, _out, asm_code.render(), {"x2", "x1"}, optimize);
}

bool CompilerContext::appendCallback(evmasm::AssemblyItem const& _i) {
	if (m_disable_rewrite) return false;
	m_disable_rewrite = true;

	auto callYUL = R"(
		// declare helper functions
		function max(first, second) -> bigger {
			bigger := first
			if gt(second, first) { bigger := second }
		}
		function min(first, second) -> smaller {
			smaller := first
			if lt(second, first) { smaller := second }
		}

		// store _gasLimit
		mstore(add(callBytes, 0x04), in_gas)
		// store _address
		mstore(add(callBytes, 0x24), addr)
		// store abi bytes memory offset
		mstore(add(callBytes, 0x44), 0x60)
		// store bytes memory _calldata.length
		mstore(add(callBytes, 0x64), argsLength)
		// store bytes memory _calldata raw data
		let rawCallBytes := add(callBytes, 0x84)
		for { let ptr := 0 } lt(ptr, argsLength) { ptr := add(ptr, 0x20) } {
			mstore(add(rawCallBytes, ptr), mload(add(argsOffset, ptr)))
		}
		// kall, only grabbing 3 words of returndata (success & abi encoding params) and just throw on top of where we put it (successfull kall will awlays return >= 0x60 bytes)
		// overpad calldata by a word (argsLen [raw data] + 0x84 [abi prefixing] + 0x20 [1 word max to pad] = argsLen + 0xa4) to ensure sufficient right 0-padding for abi encoding
		kall(callBytes, add(0xa4, argsLength), callBytes, 0x60)
		// get _success
		let wasSuccess := mload(callBytes)
		// get abi length of _data output by EM
		let returnedDataLengthFromABI := mload(add(callBytes, 0x40))

		// call identity precompile with ALL raw returndata (ignores bool and abi) to make returndatasize() correct.
		// also copies the relevant data back to the CALL's intended vals (retOffset, retLength)
		returndatacopy(callBytes, 0, returndatasize())
		kopy(add(callBytes, 0x60), returnedDataLengthFromABI, retOffset, retLength)
		// remove all the stuff we did at callbytes
		let newMemSize := msize()

		// overwrite zeros starting from either the pre-modification msize, or the end of returndata (whichever is bigger)
		let endOfReturnData := add(retOffset,min(returndatasize(), retLength))
		for { let ptr := max(callBytes, endOfReturnData) } lt(ptr, newMemSize) { ptr := add(ptr, 0x20) } {
			mstore(ptr, 0x00)
		}
		// set the first stack element out, this looks weird but it's really saying this is the intended stack output of the replaced EVM operation
		retLength := wasSuccess
	})"; 

	if (_i.type() == PushData) {
		auto dat = assemblyPtr()->data(_i.data());
		if (std::find(dat.begin(), dat.end(), 0x5b) != dat.end()) {
			m_errorReporter.warning(
				7608_error,
				assemblyPtr()->currentSourceLocation(),
				"OVM: JUMPDEST found in constant");
		}
	}

	bool ret = false;
	if (_i.type() == Operation) {
		ret = true;  // will be set to false again if we don't change the instruction
		switch (_i.instruction()) {
			case Instruction::SELFBALANCE:
			case Instruction::BALANCE:
				m_errorReporter.warning(
					1633_error,
					assemblyPtr()->currentSourceLocation(),
					"OVM: " +
					instructionInfo(_i.instruction()).name +
					" is not implemented in the OVM. (We have no native ETH -- use deposited WETH instead!)"
				);
				ret = false;
				break;
			case Instruction::BLOCKHASH:
			case Instruction::CALLCODE:
			case Instruction::COINBASE:
			case Instruction::DIFFICULTY:
			case Instruction::GASPRICE:
			case Instruction::ORIGIN:
			case Instruction::SELFDESTRUCT:
				m_errorReporter.warning(
					6388_error,
					assemblyPtr()->currentSourceLocation(),
					"OVM: " +
					instructionInfo(_i.instruction()).name +
					" is not implemented in the OVM."
				);
				ret = false;
				break;
			case Instruction::SSTORE:
				simpleRewrite("ovmSSTORE(bytes32,bytes32)", 2, 0);
				break;
			case Instruction::SLOAD:
				simpleRewrite("ovmSLOAD(bytes32)", 1, 1);
				break;
			case Instruction::EXTCODESIZE:
				simpleRewrite("ovmEXTCODESIZE(address)", 1, 1);
				break;
			case Instruction::EXTCODEHASH:
				simpleRewrite("ovmEXTCODEHASH(address)", 1, 1);
				break;
			case Instruction::CALLER:
				simpleRewrite("ovmCALLER()", 0, 1);
				break;
			case Instruction::ADDRESS:
				// address doesn't like to be optimized for some reason
				// a very small price to pay
				simpleRewrite("ovmADDRESS()", 0, 1, false);
				break;
			case Instruction::TIMESTAMP:
				simpleRewrite("ovmTIMESTAMP()", 0, 1);
				break;
			case Instruction::NUMBER:
				simpleRewrite("ovmNUMBER()", 0, 1);
				break;
			case Instruction::CHAINID:
				simpleRewrite("ovmCHAINID()", 0, 1);
				break;
			case Instruction::GASLIMIT:
				simpleRewrite("ovmGASLIMIT()", 0, 1);
				break;
			case Instruction::CALL:
				complexRewrite("ovmCALL(uint256,address,bytes)", 7, 1, callYUL,
					{"retLength", "retOffset", "argsLength", "argsOffset", "value", "addr", "in_gas"});
				break;
			case Instruction::STATICCALL:
				complexRewrite("ovmSTATICCALL(uint256,address,bytes)", 6, 1, callYUL,
					{"retLength", "retOffset", "argsLength", "argsOffset", "addr", "in_gas"});
				break;
			case Instruction::DELEGATECALL:
				complexRewrite("ovmDELEGATECALL(uint256,address,bytes)", 6, 1, callYUL,
					{"retLength", "retOffset", "argsLength", "argsOffset", "addr", "in_gas"});
				break;
			case Instruction::REVERT:
				complexRewrite("ovmREVERT(bytes)", 2, 0, R"(
						// methodId is stored for us at callBytes
						let dataStart := add(callBytes, 4)
						// store abi offset
						mstore(dataStart, 0x20)
						// store abi length
						mstore(add(dataStart, 0x20), length)
						// store bytecode itself
						for { let ptr := 0 } lt(ptr, length) { ptr := add(ptr, 0x20) } {
							mstore(add(add(dataStart, 0x40), ptr), mload(add(offset, ptr)))
						}
						// technically 0x44 is the minimum needed to add to length, but ABI wants right-padding so we overpad by 0x20.
						kall(callBytes, add(0x64, length), callBytes, 0x20)
						// kall to ovmREVERT will itself trigger safe reversion so nothing further needed! 
					})",
					{"length", "offset"});
				break;
			case Instruction::CREATE:
				complexRewrite("ovmCREATE(bytes)", 3, 1, R"(
						// methodId is stored for us at callBytes
						let dataStart := add(callBytes, 4)
						// store abi offset
						mstore(dataStart, 0x20)
						// store abi length
						mstore(add(dataStart, 0x20), length)
						// store bytecode itself
						for { let ptr := 0 } lt(ptr, length) { ptr := add(ptr, 0x20) } {
							mstore(add(add(dataStart, 0x40), ptr), mload(add(offset, ptr)))
						}
						// technically 0x44 is the minimum needed to add to length, but ABI wants right-padding so we overpad by 0x20.
						kall(callBytes, add(0x64, length), callBytes, 0x20)
						// legnth is first stack val in ==> first stack val out (address)
						length := mload(callBytes)

						// remove all the stuff we did at callbytes.
						let newMemSize := msize()
						for { let ptr := callBytes } lt(ptr, newMemSize) { ptr := add(ptr, 0x20) } {
							mstore(ptr, 0x00)
						}
					})",
					{"length", "offset", "value"});
				break;
			case Instruction::CREATE2:
				complexRewrite("ovmCREATE2(bytes,bytes32)", 4, 1, R"(
						// methodId is stored for us at callBytes
						let dataStart := add(callBytes, 4)
						// store abi offset
						mstore(dataStart, 0x40)
						// store salt
						mstore(add(dataStart, 0x20), salt)
						// store abi length
						mstore(add(dataStart, 0x40), length)
						// store bytecode itself
						for { let ptr := 0 } lt(ptr, length) { ptr := add(ptr, 0x20) } {
							mstore(add(add(dataStart, 0x60), ptr), mload(add(offset, ptr)))
						}
						// technically 0x64 is the minimum needed to add to length, but ABI wants right-padding so we overpad by 0x20.
						kall(callBytes, add(0x84, length), callBytes, 0x20)
						// salt is first stack val in ==> first stack val out (address)
						salt := mload(callBytes)

						// remove all the stuff we did at callbytes.
						let newMemSize := msize()
						for { let ptr := callBytes } lt(ptr, newMemSize) { ptr := add(ptr, 0x20) } {
							mstore(ptr, 0x00)
						}
					})",
					{"salt", "length", "offset", "value"});
				break;
			case Instruction::EXTCODECOPY:
				complexRewrite("ovmEXTCODECOPY(address,uint256,uint256)", 4, 0, R"(
						mstore(add(callBytes, 4), addr)
						mstore(add(callBytes, 0x24), offset)
						mstore(add(callBytes, 0x44), length)
						kall(callBytes, 0x64, destOffset, length)
						
						// remove all the stuff we did at callbytes, except for any part of the copied code itself which extended past callbytes.
						let newMemSize := msize()
						for { let ptr := max(callBytes, add(destOffset, length)) } lt(ptr, newMemSize) { ptr := add(ptr, 0x20) } {
							mstore(ptr, 0x00)
						}
					})",
					{"length", "offset", "destOffset", "addr"});
				break;
			case Instruction::RETURNDATACOPY:
			case Instruction::RETURNDATASIZE:
				if (m_is_building_user_asm) {
					m_errorReporter.warning(
						7742_error,
						assemblyPtr()->currentSourceLocation(),
						"OVM: Using RETURNDATASIZE or RETURNDATACOPY in user asm isn't guaranteed to work");
				}
				ret = false;
				break;
			default:
				ret = false;
				break;
		}
	}

	m_disable_rewrite = false;
	return ret;
}

void CompilerContext::addStateVariable(
	VariableDeclaration const& _declaration,
	u256 const& _storageOffset,
	unsigned _byteOffset
)
{
	m_stateVariables[&_declaration] = make_pair(_storageOffset, _byteOffset);
}

void CompilerContext::addImmutable(VariableDeclaration const& _variable)
{
	solAssert(_variable.immutable(), "Attempted to register a non-immutable variable as immutable.");
	solUnimplementedAssert(_variable.annotation().type->isValueType(), "Only immutable variables of value type are supported.");
	solAssert(m_runtimeContext, "Attempted to register an immutable variable for runtime code generation.");
	m_immutableVariables[&_variable] = CompilerUtils::generalPurposeMemoryStart + *m_reservedMemory;
	solAssert(_variable.annotation().type->memoryHeadSize() == 32, "Memory writes might overlap.");
	*m_reservedMemory += _variable.annotation().type->memoryHeadSize();
}

size_t CompilerContext::immutableMemoryOffset(VariableDeclaration const& _variable) const
{
	solAssert(m_immutableVariables.count(&_variable), "Memory offset of unknown immutable queried.");
	solAssert(m_runtimeContext, "Attempted to fetch the memory offset of an immutable variable during runtime code generation.");
	return m_immutableVariables.at(&_variable);
}

vector<string> CompilerContext::immutableVariableSlotNames(VariableDeclaration const& _variable)
{
	string baseName = to_string(_variable.id());
	solAssert(_variable.annotation().type->sizeOnStack() > 0, "");
	if (_variable.annotation().type->sizeOnStack() == 1)
		return {baseName};
	vector<string> names;
	auto collectSlotNames = [&](string const& _baseName, TypePointer type, auto const& _recurse) -> void {
		for (auto const& [slot, type]: type->stackItems())
			if (type)
				_recurse(_baseName + " " + slot, type, _recurse);
			else
				names.emplace_back(_baseName);
	};
	collectSlotNames(baseName, _variable.annotation().type, collectSlotNames);
	return names;
}

size_t CompilerContext::reservedMemory()
{
	solAssert(m_reservedMemory.has_value(), "Reserved memory was used before ");
	size_t reservedMemory = *m_reservedMemory;
	m_reservedMemory = std::nullopt;
	return reservedMemory;
}

void CompilerContext::startFunction(Declaration const& _function)
{
	m_functionCompilationQueue.startFunction(_function);
	*this << functionEntryLabel(_function);
}

void CompilerContext::callLowLevelFunction(
	string const& _name,
	unsigned _inArgs,
	unsigned _outArgs,
	function<void(CompilerContext&)> const& _generator
)
{
	evmasm::AssemblyItem retTag = pushNewTag();
	CompilerUtils(*this).moveIntoStack(_inArgs);

	*this << lowLevelFunctionTag(_name, _inArgs, _outArgs, _generator);

	appendJump(evmasm::AssemblyItem::JumpType::IntoFunction);
	adjustStackOffset(static_cast<int>(_outArgs) - 1 - static_cast<int>(_inArgs));
	*this << retTag.tag();
}

void CompilerContext::callYulFunction(
	string const& _name,
	unsigned _inArgs,
	unsigned _outArgs
)
{
	m_externallyUsedYulFunctions.insert(_name);
	auto const retTag = pushNewTag();
	CompilerUtils(*this).moveIntoStack(_inArgs);
	appendJumpTo(namedTag(_name), evmasm::AssemblyItem::JumpType::IntoFunction);
	adjustStackOffset(static_cast<int>(_outArgs) - 1 - static_cast<int>(_inArgs));
	*this << retTag.tag();
}

evmasm::AssemblyItem CompilerContext::lowLevelFunctionTag(
	string const& _name,
	unsigned _inArgs,
	unsigned _outArgs,
	function<void(CompilerContext&)> const& _generator
)
{
	auto it = m_lowLevelFunctions.find(_name);
	if (it == m_lowLevelFunctions.end())
	{
		evmasm::AssemblyItem tag = newTag().pushTag();
		m_lowLevelFunctions.insert(make_pair(_name, tag));
		m_lowLevelFunctionGenerationQueue.push(make_tuple(_name, _inArgs, _outArgs, _generator));
		return tag;
	}
	else
		return it->second;
}

void CompilerContext::appendMissingLowLevelFunctions()
{
	while (!m_lowLevelFunctionGenerationQueue.empty())
	{
		string name;
		unsigned inArgs;
		unsigned outArgs;
		function<void(CompilerContext&)> generator;
		tie(name, inArgs, outArgs, generator) = m_lowLevelFunctionGenerationQueue.front();
		m_lowLevelFunctionGenerationQueue.pop();

		setStackOffset(static_cast<int>(inArgs) + 1);
		*this << m_lowLevelFunctions.at(name).tag();
		generator(*this);
		CompilerUtils(*this).moveToStackTop(outArgs);
		appendJump(evmasm::AssemblyItem::JumpType::OutOfFunction);
		solAssert(stackHeight() == outArgs, "Invalid stack height in low-level function " + name + ".");
	}
}

pair<string, set<string>> CompilerContext::requestedYulFunctions()
{
	solAssert(!m_requestedYulFunctionsRan, "requestedYulFunctions called more than once.");
	m_requestedYulFunctionsRan = true;

	set<string> empty;
	swap(empty, m_externallyUsedYulFunctions);
	return {m_yulFunctionCollector.requestedFunctions(), std::move(empty)};
}

void CompilerContext::addVariable(
	VariableDeclaration const& _declaration,
	unsigned _offsetToCurrent
)
{
	solAssert(m_asm->deposit() >= 0 && unsigned(m_asm->deposit()) >= _offsetToCurrent, "");
	unsigned sizeOnStack = _declaration.annotation().type->sizeOnStack();
	// Variables should not have stack size other than [1, 2],
	// but that might change when new types are introduced.
	solAssert(sizeOnStack == 1 || sizeOnStack == 2, "");
	m_localVariables[&_declaration].push_back(unsigned(m_asm->deposit()) - _offsetToCurrent);
}

void CompilerContext::removeVariable(Declaration const& _declaration)
{
	solAssert(m_localVariables.count(&_declaration) && !m_localVariables[&_declaration].empty(), "");
	m_localVariables[&_declaration].pop_back();
	if (m_localVariables[&_declaration].empty())
		m_localVariables.erase(&_declaration);
}

void CompilerContext::removeVariablesAboveStackHeight(unsigned _stackHeight)
{
	vector<Declaration const*> toRemove;
	for (auto _var: m_localVariables)
	{
		solAssert(!_var.second.empty(), "");
		solAssert(_var.second.back() <= stackHeight(), "");
		if (_var.second.back() >= _stackHeight)
			toRemove.push_back(_var.first);
	}
	for (auto _var: toRemove)
		removeVariable(*_var);
}

unsigned CompilerContext::numberOfLocalVariables() const
{
	return m_localVariables.size();
}

shared_ptr<evmasm::Assembly> CompilerContext::compiledContract(ContractDefinition const& _contract) const
{
	auto ret = m_otherCompilers.find(&_contract);
	solAssert(ret != m_otherCompilers.end(), "Compiled contract not found.");
	return ret->second->assemblyPtr();
}

shared_ptr<evmasm::Assembly> CompilerContext::compiledContractRuntime(ContractDefinition const& _contract) const
{
	auto ret = m_otherCompilers.find(&_contract);
	solAssert(ret != m_otherCompilers.end(), "Compiled contract not found.");
	return ret->second->runtimeAssemblyPtr();
}

bool CompilerContext::isLocalVariable(Declaration const* _declaration) const
{
	return !!m_localVariables.count(_declaration);
}

evmasm::AssemblyItem CompilerContext::functionEntryLabel(Declaration const& _declaration)
{
	return m_functionCompilationQueue.entryLabel(_declaration, *this);
}

evmasm::AssemblyItem CompilerContext::functionEntryLabelIfExists(Declaration const& _declaration) const
{
	return m_functionCompilationQueue.entryLabelIfExists(_declaration);
}

FunctionDefinition const& CompilerContext::superFunction(FunctionDefinition const& _function, ContractDefinition const& _base)
{
	solAssert(m_mostDerivedContract, "No most derived contract set.");
	ContractDefinition const* super = _base.superContract(mostDerivedContract());
	solAssert(super, "Super contract not available.");
	return _function.resolveVirtual(mostDerivedContract(), super);
}

ContractDefinition const& CompilerContext::mostDerivedContract() const
{
	solAssert(m_mostDerivedContract, "Most derived contract not set.");
	return *m_mostDerivedContract;
}

Declaration const* CompilerContext::nextFunctionToCompile() const
{
	return m_functionCompilationQueue.nextFunctionToCompile();
}

unsigned CompilerContext::baseStackOffsetOfVariable(Declaration const& _declaration) const
{
	auto res = m_localVariables.find(&_declaration);
	solAssert(res != m_localVariables.end(), "Variable not found on stack.");
	solAssert(!res->second.empty(), "");
	return res->second.back();
}

unsigned CompilerContext::baseToCurrentStackOffset(unsigned _baseOffset) const
{
	return static_cast<unsigned>(m_asm->deposit()) - _baseOffset - 1;
}

unsigned CompilerContext::currentToBaseStackOffset(unsigned _offset) const
{
	return static_cast<unsigned>(m_asm->deposit()) - _offset - 1;
}

pair<u256, unsigned> CompilerContext::storageLocationOfVariable(Declaration const& _declaration) const
{
	auto it = m_stateVariables.find(&_declaration);
	solAssert(it != m_stateVariables.end(), "Variable not found in storage.");
	return it->second;
}

CompilerContext& CompilerContext::appendJump(evmasm::AssemblyItem::JumpType _jumpType)
{
	evmasm::AssemblyItem item(Instruction::JUMP);
	item.setJumpType(_jumpType);
	return *this << item;
}

CompilerContext& CompilerContext::appendInvalid()
{
	return *this << Instruction::INVALID;
}

CompilerContext& CompilerContext::appendConditionalInvalid()
{
	*this << Instruction::ISZERO;
	evmasm::AssemblyItem afterTag = appendConditionalJump();
	*this << Instruction::INVALID;
	*this << afterTag;
	return *this;
}

CompilerContext& CompilerContext::appendRevert(string const& _message)
{
	appendInlineAssembly("{ " + revertReasonIfDebug(_message) + " }");
	return *this;
}

CompilerContext& CompilerContext::appendConditionalRevert(bool _forwardReturnData, string const& _message)
{
	if (_forwardReturnData && m_evmVersion.supportsReturndata())
		appendInlineAssembly(R"({
			if condition {
				returndatacopy(0, 0, returndatasize())
				revert(0, returndatasize())
			}
		})", {"condition"});
	else
		appendInlineAssembly("{ if condition { " + revertReasonIfDebug(_message) + " } }", {"condition"});
	*this << Instruction::POP;
	return *this;
}

void CompilerContext::resetVisitedNodes(ASTNode const* _node)
{
	stack<ASTNode const*> newStack;
	newStack.push(_node);
	std::swap(m_visitedNodes, newStack);
	updateSourceLocation();
}

void CompilerContext::appendInlineAssembly(
	string const& _assembly,
	vector<string> const& _localVariables,
	set<string> const& _externallyUsedFunctions,
	bool _system,
	OptimiserSettings const& _optimiserSettings
)
{
	unsigned startStackHeight = stackHeight();

	set<yul::YulString> externallyUsedIdentifiers;
	for (auto const& fun: _externallyUsedFunctions)
		externallyUsedIdentifiers.insert(yul::YulString(fun));
	for (auto const& var: _localVariables)
		externallyUsedIdentifiers.insert(yul::YulString(var));

	yul::ExternalIdentifierAccess identifierAccess;
	identifierAccess.resolve = [&](
		yul::Identifier const& _identifier,
		yul::IdentifierContext,
		bool _insideFunction
	) -> bool
	{
		if (_insideFunction)
			return false;
		return contains(_localVariables, _identifier.name.str());
	};
	identifierAccess.generateCode = [&](
		yul::Identifier const& _identifier,
		yul::IdentifierContext _context,
		yul::AbstractAssembly& _assembly
	)
	{
		auto it = std::find(_localVariables.begin(), _localVariables.end(), _identifier.name.str());
		solAssert(it != _localVariables.end(), "");
		auto stackDepth = static_cast<size_t>(distance(it, _localVariables.end()));
		size_t stackDiff = static_cast<size_t>(_assembly.stackHeight()) - startStackHeight + stackDepth;
		if (_context == yul::IdentifierContext::LValue)
			stackDiff -= 1;
		if (stackDiff < 1 || stackDiff > 16)
			BOOST_THROW_EXCEPTION(
				StackTooDeepError() <<
				errinfo_sourceLocation(_identifier.location) <<
				util::errinfo_comment("Stack too deep (" + to_string(stackDiff) + "), try removing local variables.")
			);
		if (_context == yul::IdentifierContext::RValue)
			_assembly.appendInstruction(dupInstruction(stackDiff));
		else
		{
			_assembly.appendInstruction(swapInstruction(stackDiff));
			_assembly.appendInstruction(Instruction::POP);
		}
	};

	ErrorList errors;
	ErrorReporter errorReporter(errors);
	auto scanner = make_shared<langutil::Scanner>(langutil::CharStream(_assembly, "--CODEGEN--"));
	yul::EVMDialect const& dialect = yul::EVMDialect::strictAssemblyForEVM(m_evmVersion);
	optional<langutil::SourceLocation> locationOverride;
	if (!_system)
		locationOverride = m_asm->currentSourceLocation();
	shared_ptr<yul::Block> parserResult =
		yul::Parser(errorReporter, dialect, std::move(locationOverride))
		.parse(scanner, false);
#ifdef SOL_OUTPUT_ASM
	cout << yul::AsmPrinter(&dialect)(*parserResult) << endl;
#endif

	auto reportError = [&](string const& _context)
	{
		string message =
			"Error parsing/analyzing inline assembly block:\n" +
			_context + "\n"
			"------------------ Input: -----------------\n" +
			_assembly + "\n"
			"------------------ Errors: ----------------\n";
		for (auto const& error: errorReporter.errors())
			message += SourceReferenceFormatter::formatErrorInformation(*error);
		message += "-------------------------------------------\n";

		solAssert(false, message);
	};

	yul::AsmAnalysisInfo analysisInfo;
	bool analyzerResult = false;
	if (parserResult)
		analyzerResult = yul::AsmAnalyzer(
			analysisInfo,
			errorReporter,
			dialect,
			identifierAccess.resolve
		).analyze(*parserResult);
	if (!parserResult || !errorReporter.errors().empty() || !analyzerResult)
		reportError("Invalid assembly generated by code generator.");

	// Several optimizer steps cannot handle externally supplied stack variables,
	// so we essentially only optimize the ABI functions.
	if (_optimiserSettings.runYulOptimiser && _localVariables.empty())
	{
		yul::Object obj;
		obj.code = parserResult;
		obj.analysisInfo = make_shared<yul::AsmAnalysisInfo>(analysisInfo);

		optimizeYul(obj, dialect, _optimiserSettings, externallyUsedIdentifiers);

		analysisInfo = std::move(*obj.analysisInfo);
		parserResult = std::move(obj.code);

#ifdef SOL_OUTPUT_ASM
		cout << "After optimizer:" << endl;
		cout << yul::AsmPrinter(&dialect)(*parserResult) << endl;
#endif
	}

	if (!errorReporter.errors().empty())
		reportError("Failed to analyze inline assembly block.");

	solAssert(errorReporter.errors().empty(), "Failed to analyze inline assembly block.");
	yul::CodeGenerator::assemble(
		*parserResult,
		analysisInfo,
		*m_asm,
		m_evmVersion,
		identifierAccess,
		_system,
		_optimiserSettings.optimizeStackAllocation
	);

	// Reset the source location to the one of the node (instead of the CODEGEN source location)
	updateSourceLocation();
}


void CompilerContext::optimizeYul(yul::Object& _object, yul::EVMDialect const& _dialect, OptimiserSettings const& _optimiserSettings, std::set<yul::YulString> const& _externalIdentifiers)
{
#ifdef SOL_OUTPUT_ASM
	cout << yul::AsmPrinter(*dialect)(*_object.code) << endl;
#endif

	bool const isCreation = runtimeContext() != nullptr;
	yul::GasMeter meter(_dialect, isCreation, _optimiserSettings.expectedExecutionsPerDeployment);
	yul::OptimiserSuite::run(
		_dialect,
		&meter,
		_object,
		_optimiserSettings.optimizeStackAllocation,
		_optimiserSettings.yulOptimiserSteps,
		_externalIdentifiers
	);

#ifdef SOL_OUTPUT_ASM
	cout << "After optimizer:" << endl;
	cout << yul::AsmPrinter(*dialect)(*object.code) << endl;
#endif
}

LinkerObject const& CompilerContext::assembledObject() const
{
	LinkerObject const& object = m_asm->assemble();
	solAssert(object.immutableReferences.empty(), "Leftover immutables.");
	return object;
}

string CompilerContext::revertReasonIfDebug(string const& _message)
{
	return YulUtilFunctions::revertReasonIfDebug(m_revertStrings, _message);
}

void CompilerContext::updateSourceLocation()
{
	m_asm->setSourceLocation(m_visitedNodes.empty() ? SourceLocation() : m_visitedNodes.top()->location());
}

evmasm::Assembly::OptimiserSettings CompilerContext::translateOptimiserSettings(OptimiserSettings const& _settings)
{
	// Constructing it this way so that we notice changes in the fields.
	evmasm::Assembly::OptimiserSettings asmSettings{false, false, false, false, false, false, m_evmVersion, 0};
	asmSettings.isCreation = true;
	asmSettings.runJumpdestRemover = _settings.runJumpdestRemover;
	asmSettings.runPeephole = _settings.runPeephole;
	asmSettings.runDeduplicate = _settings.runDeduplicate;
	asmSettings.runCSE = _settings.runCSE;
	asmSettings.runConstantOptimiser = _settings.runConstantOptimiser;
	asmSettings.expectedExecutionsPerDeployment = _settings.expectedExecutionsPerDeployment;
	asmSettings.evmVersion = m_evmVersion;
	return asmSettings;
}

evmasm::AssemblyItem CompilerContext::FunctionCompilationQueue::entryLabel(
	Declaration const& _declaration,
	CompilerContext& _context
)
{
	auto res = m_entryLabels.find(&_declaration);
	if (res == m_entryLabels.end())
	{
		evmasm::AssemblyItem tag(_context.newTag());
		m_entryLabels.insert(make_pair(&_declaration, tag));
		m_functionsToCompile.push(&_declaration);
		return tag.tag();
	}
	else
		return res->second.tag();

}

evmasm::AssemblyItem CompilerContext::FunctionCompilationQueue::entryLabelIfExists(Declaration const& _declaration) const
{
	auto res = m_entryLabels.find(&_declaration);
	return res == m_entryLabels.end() ? evmasm::AssemblyItem(evmasm::UndefinedItem) : res->second.tag();
}

Declaration const* CompilerContext::FunctionCompilationQueue::nextFunctionToCompile() const
{
	while (!m_functionsToCompile.empty())
	{
		if (m_alreadyCompiledFunctions.count(m_functionsToCompile.front()))
			m_functionsToCompile.pop();
		else
			return m_functionsToCompile.front();
	}
	return nullptr;
}

void CompilerContext::FunctionCompilationQueue::startFunction(Declaration const& _function)
{
	if (!m_functionsToCompile.empty() && m_functionsToCompile.front() == &_function)
		m_functionsToCompile.pop();
	m_alreadyCompiledFunctions.insert(&_function);
}
