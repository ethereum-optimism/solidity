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
 * @author Alex Beregszaszi
 * Removes unused JUMPDESTs.
 */

#include <libevmasm/JumpdestRemover.h>

#include <libevmasm/AssemblyItem.h>

#include <iostream>

using namespace std;
using namespace solidity;
using namespace solidity::util;
using namespace solidity::evmasm;

bool JumpdestRemover::optimise(set<size_t> const& _tagsReferencedFromOutside)
{
	set<size_t> references{referencedTags(m_items, numeric_limits<size_t>::max())};
	references.insert(_tagsReferencedFromOutside.begin(), _tagsReferencedFromOutside.end());

	size_t initialSize = m_items.size();
	/// Remove tags which are never referenced.
	auto pend = remove_if(
		m_items.begin(),
		m_items.end(),
		[&](AssemblyItem const& _item)
		{
			if (_item.type() != Tag)
				return false;
			auto asmIdAndTag = _item.splitForeignPushTag();
			assertThrow(asmIdAndTag.first == numeric_limits<size_t>::max(), OptimizerException, "Sub-assembly tag used as label.");
			size_t tag = asmIdAndTag.second;
			// cerr << "!references.count(tag) for tag " << tag << " is: " << !references.count(tag) << endl;
			return !references.count(tag);
		}
	);
	// cerr << "erasing some unused jumpdests (from " << pend.base(). << "to " << m_items.end().base()->tag() << endl;
	m_items.erase(pend, m_items.end());
	
	return m_items.size() != initialSize;
}

set<size_t> JumpdestRemover::referencedTags(AssemblyItems const& _items, size_t _subId)
{
	set<size_t> ret;

	for(std::size_t i=0; i<_items.size(); ++i)  
	// for (auto const& item: _items)
	{
		auto item = _items[i];
		if (item.type() == PushTag)
		{
			auto subAndTag = item.splitForeignPushTag();
			if (subAndTag.first == _subId)
				ret.insert(subAndTag.second);
		}
		// BEGIN: OVM CHANGES.  Allows us to identify tags as referenced if they appear in PC PUSH ADD JUMP so they are not incorrectly removed.
		if (item.type() == Operation)
		{
			if (item.instruction() == solidity::evmasm::Instruction::PC)
			{
				auto addedToPC = _items[i+1].data();
				// matched first PC in "safe" pattern
				if (addedToPC == 29)
				{
					// cerr << "found first PC in kall(), the tagged jumpdests are: ";
					cerr << "found PC with data of next item of " << addedToPC << endl;
					cerr << "the 18th item after this is: " << _items[i+18];
					auto firstJumpdestSubAndTag = _items[i+18].splitForeignPushTag();
					ret.insert(firstJumpdestSubAndTag.second);
					// cerr << "the next few opcodes are: ";
					// for(size_t j=0; j < 30; ++j) {
					// 	cerr << _items[i+j];
					// }				
					cerr << endl << " and the opcode at that offset is" << _items[i+addedToPC.convert_to<size_t>()] << endl;
				}
			}
		}
	}
	return ret;
}
