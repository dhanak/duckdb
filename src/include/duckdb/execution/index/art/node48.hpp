//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/art/node48.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once
#include "duckdb/execution/index/art/node.hpp"

namespace duckdb {

class Node48 : public Node {
public:
	explicit Node48(size_t compression_length);
	uint8_t child_index[256];

	SwizzleablePointer children[48];

public:
	//! Get position of a byte, returns -1 if not exists
	idx_t GetChildPos(uint8_t k) override;
	//! Get the position of the first child that is greater or equal to the specific byte, or DConstants::INVALID_INDEX
	//! if there are no children matching the criteria
	idx_t GetChildGreaterEqual(uint8_t k, bool &equal) override;
	//! Get the next position in the node, or DConstants::INVALID_INDEX if there is no next position
	idx_t GetNextPos(idx_t pos) override;
	//! Get Node48 Child
	Node *GetChild(ART &art, idx_t pos) override;

	idx_t GetMin() override;
	//! Replace child pointer
	void ReplaceChildPointer(idx_t pos, Node *node) override;

	//! Insert a new child node at key_byte into the Node48
	static void Insert(Node *&node, uint8_t key_byte, Node *new_child);

	//! Shrink to node 16
	static void Erase(Node *&node, int pos, ART &art);

	//! Merge two nodes with matching prefixes
	static void Merge(Node *l_node, Node *r_node, idx_t depth);
};
} // namespace duckdb
