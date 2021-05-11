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

#pragma once

#include <libevmasm/Instruction.h>
#include <liblangutil/SourceLocation.h>
#include <libevmasm/AssemblyItem.h>
#include <libevmasm/LinkerObject.h>
#include <libevmasm/Exceptions.h>

#include <liblangutil/EVMVersion.h>

#include <libsolutil/Common.h>
#include <libsolutil/Assertions.h>
#include <libsolutil/Keccak256.h>

#include <json/json.h>

#include <iostream>
#include <sstream>
#include <memory>

namespace solidity::evmasm
{

using AssemblyPointer = std::shared_ptr<Assembly>;

class Assembly
{
public:
	explicit Assembly(std::string _name = std::string()):m_name(std::move(_name)) { }

	AssemblyItem newTag() { assertThrow(m_usedTags < 0xffffffff, AssemblyException, ""); return AssemblyItem(Tag, m_usedTags++); }
	AssemblyItem newPushTag() { assertThrow(m_usedTags < 0xffffffff, AssemblyException, ""); return AssemblyItem(PushTag, m_usedTags++); }
	/// Returns a tag identified by the given name. Creates it if it does not yet exist.
	AssemblyItem namedTag(std::string const& _name);
	AssemblyItem newData(bytes const& _data) { util::h256 h(util::keccak256(util::asString(_data))); m_data[h] = _data; return AssemblyItem(PushData, h); }
	bytes const& data(util::h256 const& _i) const { return m_data.at(_i); }
	AssemblyItem newSub(AssemblyPointer const& _sub) { m_subs.push_back(_sub); return AssemblyItem(PushSub, m_subs.size() - 1); }
	Assembly const& sub(size_t _sub) const { return *m_subs.at(_sub); }
	Assembly& sub(size_t _sub) { return *m_subs.at(_sub); }
	size_t numSubs() const { return m_subs.size(); }
	AssemblyItem newPushSubSize(u256 const& _subId) { return AssemblyItem(PushSubSize, _subId); }
	AssemblyItem newPushLibraryAddress(std::string const& _identifier);
	AssemblyItem newPushImmutable(std::string const& _identifier);
	AssemblyItem newImmutableAssignment(std::string const& _identifier);

	AssemblyItem const& append(AssemblyItem const& _i);
	AssemblyItem const& append(bytes const& _data) { return append(newData(_data)); }

	template <class T> Assembly& operator<<(T const& _d) { append(_d); return *this; }

	/// Pushes the final size of the current assembly itself. Use this when the code is modified
	/// after compilation and CODESIZE is not an option.
	void appendProgramSize() { append(AssemblyItem(PushProgramSize)); }
	void appendLibraryAddress(std::string const& _identifier) { append(newPushLibraryAddress(_identifier)); }
	void appendImmutable(std::string const& _identifier) { append(newPushImmutable(_identifier)); }
	void appendImmutableAssignment(std::string const& _identifier) { append(newImmutableAssignment(_identifier)); }

	AssemblyItem appendJump() { auto ret = append(newPushTag()); append(Instruction::JUMP); return ret; }
	AssemblyItem appendJumpI() { auto ret = append(newPushTag()); append(Instruction::JUMPI); return ret; }
	AssemblyItem appendJump(AssemblyItem const& _tag) { auto ret = append(_tag.pushTag()); append(Instruction::JUMP); return ret; }
	AssemblyItem appendJumpI(AssemblyItem const& _tag) { auto ret = append(_tag.pushTag()); append(Instruction::JUMPI); return ret; }

	/// Adds a subroutine to the code (in the data section) and pushes its size (via a tag)
	/// on the stack. @returns the pushsub assembly item.
	AssemblyItem appendSubroutine(AssemblyPointer const& _assembly) { auto sub = newSub(_assembly); append(newPushSubSize(size_t(sub.data()))); return sub; }
	void pushSubroutineSize(size_t _subRoutine) { append(newPushSubSize(_subRoutine)); }
	/// Pushes the offset of the subroutine.
	void pushSubroutineOffset(size_t _subRoutine) { append(AssemblyItem(PushSub, _subRoutine)); }

	/// Appends @a _data literally to the very end of the bytecode.
	void appendAuxiliaryDataToEnd(bytes const& _data) { (void)_data; /* m_auxiliaryData += _data; */ } // OVM change: remove potentially "unsafe" auxdata
	/// Returns the assembly items.
	AssemblyItems const& items() const { return m_items; }

	/// Returns the mutable assembly items. Use with care!
	AssemblyItems& items() { return m_items; }

	int deposit() const { return m_deposit; }
	void adjustDeposit(int _adjustment) { m_deposit += _adjustment; assertThrow(m_deposit >= 0, InvalidDeposit, ""); }
	void setDeposit(int _deposit) { m_deposit = _deposit; assertThrow(m_deposit >= 0, InvalidDeposit, ""); }
	std::string const& name() const { return m_name; }

	/// Changes the source location used for each appended item.
	void setSourceLocation(langutil::SourceLocation const& _location) { m_currentSourceLocation = _location; }
	langutil::SourceLocation const& currentSourceLocation() const { return m_currentSourceLocation; }

	/// Assembles the assembly into bytecode. The assembly should not be modified after this call, since the assembled version is cached.
	LinkerObject const& assemble() const;

	struct OptimiserSettings
	{
		bool isCreation = false;
		bool runInliner = false;
		bool runJumpdestRemover = false;
		bool runPeephole = false;
		bool runDeduplicate = false;
		bool runCSE = false;
		bool runConstantOptimiser = false;
		langutil::EVMVersion evmVersion;
		/// This specifies an estimate on how often each opcode in this assembly will be executed,
		/// i.e. use a small value to optimise for size and a large value to optimise for runtime gas usage.
		size_t expectedExecutionsPerDeployment = 200;
	};

	/// Modify and return the current assembly such that creation and execution gas usage
	/// is optimised according to the settings in @a _settings.
	Assembly& optimise(OptimiserSettings const& _settings);

	/// Modify (if @a _enable is set) and return the current assembly such that creation and
	/// execution gas usage is optimised. @a _isCreation should be true for the top-level assembly.
	/// @a _runs specifes an estimate on how often each opcode in this assembly will be executed,
	/// i.e. use a small value to optimise for size and a large value to optimise for runtime.
	/// If @a _enable is not set, will perform some simple peephole optimizations.
	Assembly& optimise(bool _enable, langutil::EVMVersion _evmVersion, bool _isCreation, size_t _runs);

	/// Create a text representation of the assembly.
	std::string assemblyString(
		StringMap const& _sourceCodes = StringMap()
	) const;
	void assemblyStream(
		std::ostream& _out,
		std::string const& _prefix = "",
		StringMap const& _sourceCodes = StringMap()
	) const;

	/// Create a JSON representation of the assembly.
	Json::Value assemblyJSON(
		std::map<std::string, unsigned> const& _sourceIndices = std::map<std::string, unsigned>()
	) const;

	/// Mark this assembly as invalid. Calling ``assemble`` on it will throw.
	void markAsInvalid() { m_invalid = true; }

	std::vector<size_t> decodeSubPath(size_t _subObjectId) const;
	size_t encodeSubPath(std::vector<size_t> const& _subPath);

	/// OVM change: opcode replacement callback
	void setAppendCallback(std::function<bool(AssemblyItem const&)> f) { append_callback = f; }
	std::function<bool(AssemblyItem const&)> append_callback = NULL;

protected:
	/// Does the same operations as @a optimise, but should only be applied to a sub and
	/// returns the replaced tags. Also takes an argument containing the tags of this assembly
	/// that are referenced in a super-assembly.
	std::map<u256, u256> optimiseInternal(OptimiserSettings const& _settings, std::set<size_t> _tagsReferencedFromOutside);

	unsigned bytesRequired(unsigned subTagSize) const;

private:
	static Json::Value createJsonValue(
		std::string _name,
		int _source,
		int _begin,
		int _end,
		std::string _value = std::string(),
		std::string _jumpType = std::string()
	);
	static std::string toStringInHex(u256 _value);

	bool m_invalid = false;

	Assembly const* subAssemblyById(size_t _subId) const;

protected:
	/// 0 is reserved for exception
	unsigned m_usedTags = 1;
	std::map<std::string, size_t> m_namedTags;
	AssemblyItems m_items;
	std::map<util::h256, bytes> m_data;
	/// Data that is appended to the very end of the contract.
	bytes m_auxiliaryData;
	std::vector<std::shared_ptr<Assembly>> m_subs;
	std::map<util::h256, std::string> m_strings;
	std::map<util::h256, std::string> m_libraries; ///< Identifiers of libraries to be linked.
	std::map<util::h256, std::string> m_immutables; ///< Identifiers of immutables.

	/// Map from a vector representing a path to a particular sub assembly to sub assembly id.
	/// This map is used only for sub-assemblies which are not direct sub-assemblies (where path is having more than one value).
	std::map<std::vector<size_t>, size_t> m_subPaths;

	mutable LinkerObject m_assembledObject;
	mutable std::vector<size_t> m_tagPositionsInBytecode;

	int m_deposit = 0;
	/// Internal name of the assembly object, only used with the Yul backend
	/// currently
	std::string m_name;

	langutil::SourceLocation m_currentSourceLocation;
public:
	size_t m_currentModifierDepth = 0;
};

inline std::ostream& operator<<(std::ostream& _out, Assembly const& _a)
{
	_a.assemblyStream(_out);
	return _out;
}

}
