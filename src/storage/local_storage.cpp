#include "duckdb/transaction/local_storage.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/write_ahead_log.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/planner/table_filter.hpp"

#include "duckdb/storage/table/column_segment.hpp"

namespace duckdb {

LocalTableStorage::LocalTableStorage(DataTable &table) :
    table(table), row_groups(table.info, table.types, MAX_ROW_ID, 0), deleted_rows(0) {
	row_groups.InitializeEmpty();
	stats.InitializeEmpty(table.types);
	table.info->indexes.Scan([&](Index &index) {
		D_ASSERT(index.type == IndexType::ART);
		auto &art = (ART &)index;
		if (art.is_unique) {
			// unique index: create a local ART index that maintains the same unique constraint
			vector<unique_ptr<Expression>> unbound_expressions;
			for (auto &expr : art.unbound_expressions) {
				unbound_expressions.push_back(expr->Copy());
			}
			indexes.AddIndex(make_unique<ART>(art.column_ids, move(unbound_expressions), true));
		}
		return false;
	});
}

LocalTableStorage::~LocalTableStorage() {
}

void LocalTableStorage::InitializeScan(CollectionScanState &state, TableFilterSet *table_filters) {
	if (row_groups.GetTotalRows() == 0) {
		// nothing to scan
		return;
	}
	row_groups.InitializeScan(state, state.GetColumnIds(), table_filters);
}

idx_t LocalTableStorage::EstimatedSize() {
	idx_t appended_rows = row_groups.GetTotalRows() - deleted_rows;
	if (appended_rows == 0) {
		return 0;
	}
	idx_t row_size = 0;
	auto &types = row_groups.GetTypes();
	for (auto &type : types) {
		row_size += GetTypeIdSize(type.InternalType());
	}
	return appended_rows * row_size;
}

void LocalStorage::InitializeScan(DataTable *table, CollectionScanState &state, TableFilterSet *table_filters) {
	auto entry = table_storage.find(table);
	if (entry == table_storage.end()) {
		return;
	}
	auto storage = entry->second.get();
	storage->InitializeScan(state, table_filters);
}

void LocalStorage::Scan(CollectionScanState &state, const vector<column_t> &column_ids, DataChunk &result) {
	state.Scan(transaction, result);
}


void LocalStorage::InitializeParallelScan(DataTable *table, ParallelCollectionScanState &state) {
	auto storage = GetStorage(table);
	if (!storage) {
		state.max_row = 0;
		state.vector_index = 0;
		state.current_row_group = nullptr;
	} else {
		storage->row_groups.InitializeParallelScan(state);
	}
}

bool LocalStorage::NextParallelScan(ClientContext &context, DataTable *table, ParallelCollectionScanState &state, CollectionScanState &scan_state) {
	auto storage = GetStorage(table);
	if (!storage) {
		return false;
	}
	return storage->row_groups.NextParallelScan(context, state, scan_state);
}

void LocalStorage::Append(DataTable *table, DataChunk &chunk) {
	auto entry = table_storage.find(table);
	LocalTableStorage *storage;
	if (entry == table_storage.end()) {
		auto new_storage = make_unique<LocalTableStorage>(*table);
		storage = new_storage.get();
		table_storage.insert(make_pair(table, move(new_storage)));
	} else {
		storage = entry->second.get();
	}
	// append to unique indices (if any)
	idx_t base_id = MAX_ROW_ID + storage->row_groups.GetTotalRows();
	if (!DataTable::AppendToIndexes(storage->indexes, chunk, base_id)) {
		throw ConstraintException("PRIMARY KEY or UNIQUE constraint violated: duplicated key");
	}

	//! Append to the chunk
	TableAppendState state;
	storage->row_groups.InitializeAppend(transaction, state, chunk.size());
	storage->row_groups.Append(transaction, chunk, state, storage->stats);
}

LocalTableStorage *LocalStorage::GetStorage(DataTable *table) {
	auto entry = table_storage.find(table);
	return entry == table_storage.end() ? nullptr : entry->second.get();
}

idx_t LocalStorage::EstimatedSize() {
	idx_t estimated_size = 0;
	for (auto &storage : table_storage) {
		estimated_size += storage.second->EstimatedSize();
	}
	return estimated_size;
}

idx_t LocalStorage::Delete(DataTable *table, Vector &row_ids, idx_t count) {
	throw InternalException("FIXME: LocalStorage::Delete");
}

void LocalStorage::Update(DataTable *table, Vector &row_ids, const vector<column_t> &column_ids, DataChunk &data) {
	throw InternalException("FIXME: LocalStorage::Update");
}

template <class T>
bool LocalStorage::ScanTableStorage(DataTable &table, LocalTableStorage &storage, T &&fun) {
	vector<column_t> column_ids;
	for (idx_t i = 0; i < table.types.size(); i++) {
		column_ids.push_back(i);
	}

	DataChunk chunk;
	chunk.Initialize(table.types);

	// initialize the scan
	TableScanState state;
	state.Initialize(move(column_ids), nullptr);
	storage.InitializeScan(state.local_state, nullptr);

	while (true) {
		chunk.Reset();
		Scan(state.local_state, column_ids, chunk);
		if (chunk.size() == 0) {
			return true;
		}
		if (!fun(chunk)) {
			return false;
		}
	}
}

void LocalStorage::Flush(DataTable &table, LocalTableStorage &storage) {
	if (storage.row_groups.GetTotalRows() <= storage.deleted_rows) {
		return;
	}
	idx_t append_count = storage.row_groups.GetTotalRows() - storage.deleted_rows;
	TableAppendState append_state;
	table.InitializeAppend(transaction, append_state, append_count);

	bool constraint_violated = false;
	ScanTableStorage(table, storage, [&](DataChunk &chunk) -> bool {
		// append this chunk to the indexes of the table
		if (!table.AppendToIndexes(chunk, append_state.current_row)) {
			constraint_violated = true;
			return false;
		}
		// append to base table
		table.Append(transaction, chunk, append_state);
		return true;
	});
	if (constraint_violated) {
		// need to revert the append
		row_t current_row = append_state.row_start;
		// remove the data from the indexes, if there are any indexes
		ScanTableStorage(table, storage, [&](DataChunk &chunk) -> bool {
			// append this chunk to the indexes of the table
			table.RemoveFromIndexes(append_state, chunk, current_row);

			current_row += chunk.size();
			if (current_row >= append_state.current_row) {
				// finished deleting all rows from the index: abort now
				return false;
			}
			return true;
		});
		table.RevertAppendInternal(append_state.row_start, append_count);
		table_storage[&table].reset();
		throw ConstraintException("PRIMARY KEY or UNIQUE constraint violated: duplicated key");
	}
	table_storage[&table].reset();
	transaction.PushAppend(&table, append_state.row_start, append_count);
}

void LocalStorage::Commit(LocalStorage::CommitState &commit_state, Transaction &transaction, WriteAheadLog *log,
                          transaction_t commit_id) {
	// commit local storage, iterate over all entries in the table storage map
	for (auto &entry : table_storage) {
		auto table = entry.first;
		auto storage = entry.second.get();
		Flush(*table, *storage);
	}
	// finished commit: clear local storage
	table_storage.clear();
}


idx_t LocalStorage::AddedRows(DataTable *table) {
	auto entry = table_storage.find(table);
	if (entry == table_storage.end()) {
		return 0;
	}
	return entry->second->row_groups.GetTotalRows() - entry->second->deleted_rows;
}


void LocalStorage::AddColumn(DataTable *old_dt, DataTable *new_dt, ColumnDefinition &new_column,
                             Expression *default_value) {
	throw InternalException("FIXME: LocalStorage::AddColumn");
}

void LocalStorage::ChangeType(DataTable *old_dt, DataTable *new_dt, idx_t changed_idx, const LogicalType &target_type,
                              const vector<column_t> &bound_columns, Expression &cast_expr) {
	// check if there are any pending appends for the old version of the table
	auto entry = table_storage.find(old_dt);
	if (entry == table_storage.end()) {
		return;
	}
	throw NotImplementedException("FIXME: ALTER TYPE with transaction local data not currently supported");
}

} // namespace duckdb
