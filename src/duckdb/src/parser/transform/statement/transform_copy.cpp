#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/statement/copy_statement.hpp"
#include "duckdb/parser/transformer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"

#include <cstring>

namespace duckdb {

void Transformer::TransformCopyOptions(CopyInfo &info, duckdb_libpgquery::PGList *options) {
	if (!options) {
		return;
	}
	duckdb_libpgquery::PGListCell *cell = nullptr;

	// iterate over each option
	for_each_cell(cell, options->head) {
		auto *def_elem = reinterpret_cast<duckdb_libpgquery::PGDefElem *>(cell->data.ptr_value);
		if (StringUtil::Lower(def_elem->defname) == "format") {
			// format specifier: interpret this option
			auto *format_val = (duckdb_libpgquery::PGValue *)(def_elem->arg);
			if (!format_val || format_val->type != duckdb_libpgquery::T_PGString) {
				throw ParserException("Unsupported parameter type for FORMAT: expected e.g. FORMAT 'csv', 'parquet'");
			}
			info.format = StringUtil::Lower(format_val->val.str);
			continue;
		}
		// otherwise
		if (info.options.find(def_elem->defname) != info.options.end()) {
			throw ParserException("Unexpected duplicate option \"%s\"", def_elem->defname);
		}
		if (!def_elem->arg) {
			info.options[def_elem->defname] = vector<Value>();
			continue;
		}
		switch (def_elem->arg->type) {
		case duckdb_libpgquery::T_PGList: {
			auto column_list = (duckdb_libpgquery::PGList *)(def_elem->arg);
			for (auto c = column_list->head; c != nullptr; c = lnext(c)) {
				auto target = (duckdb_libpgquery::PGResTarget *)(c->data.ptr_value);
				info.options[def_elem->defname].push_back(Value(target->name));
			}
			break;
		}
		case duckdb_libpgquery::T_PGAStar:
			info.options[def_elem->defname].push_back(Value("*"));
			break;
		default:
			info.options[def_elem->defname].push_back(
			    TransformValue(*((duckdb_libpgquery::PGValue *)def_elem->arg))->value);
			break;
		}
	}
}

unique_ptr<CopyStatement> Transformer::TransformCopy(duckdb_libpgquery::PGNode *node) {
	auto stmt = reinterpret_cast<duckdb_libpgquery::PGCopyStmt *>(node);
	D_ASSERT(stmt);
	auto result = make_unique<CopyStatement>();
	auto &info = *result->info;

	// get file_path and is_from
	info.file_path = stmt->filename;
	info.is_from = stmt->is_from;
	info.format = "csv";

	// get select_list
	if (stmt->attlist) {
		for (auto n = stmt->attlist->head; n != nullptr; n = n->next) {
			auto target = reinterpret_cast<duckdb_libpgquery::PGResTarget *>(n->data.ptr_value);
			if (target->name) {
				info.select_list.emplace_back(target->name);
			}
		}
	}

	if (stmt->relation) {
		auto ref = TransformRangeVar(stmt->relation);
		auto &table = *reinterpret_cast<BaseTableRef *>(ref.get());
		info.table = table.table_name;
		info.schema = table.schema_name;
	} else {
		result->select_statement = TransformSelectNode((duckdb_libpgquery::PGSelectStmt *)stmt->query);
	}

	// handle the different options of the COPY statement
	TransformCopyOptions(info, stmt->options);

	return result;
}

} // namespace duckdb
