// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
//
// SQLTables' catalog is a search pattern in ODBC 3, but the server matches the catalog
// exactly: the driver filters a whole listing by the pattern (hardcharger-odbc-driver #6).

#include "chargewire_statement_get_tables.h"
#include <arrow/api.h>
#include "gtest/gtest.h"

namespace driver {
namespace chargewire_odbc {
namespace {

std::shared_ptr<arrow::Table> Listing(const std::vector<std::optional<std::string>> &catalogs) {
  arrow::StringBuilder catalog, schema, table, type;
  int n = 0;
  for (const auto &name : catalogs) {
    EXPECT_TRUE((name ? catalog.Append(*name) : catalog.AppendNull()).ok());
    EXPECT_TRUE(schema.Append("main").ok());
    EXPECT_TRUE(table.Append("t" + std::to_string(n++)).ok());
    EXPECT_TRUE(type.Append("BASE TABLE").ok());
  }
  std::shared_ptr<arrow::Array> c, s, t, y;
  EXPECT_TRUE(catalog.Finish(&c).ok());
  EXPECT_TRUE(schema.Finish(&s).ok());
  EXPECT_TRUE(table.Finish(&t).ok());
  EXPECT_TRUE(type.Finish(&y).ok());
  auto schema_ = arrow::schema({arrow::field("catalog_name", arrow::utf8()),
                                arrow::field("db_schema_name", arrow::utf8()),
                                arrow::field("table_name", arrow::utf8(), false),
                                arrow::field("table_type", arrow::utf8(), false)});
  return arrow::Table::Make(schema_, {c, s, t, y});
}

std::shared_ptr<ResultInfo> Info(const std::shared_ptr<arrow::Table> &table) {
  auto info = std::make_shared<ResultInfo>(table->schema());
  info->reader = std::make_shared<arrow::TableBatchReader>(table);
  info->rows = table->num_rows();
  return info;
}

std::vector<std::string> Tables(const std::shared_ptr<ResultInfo> &info) {
  auto table = info->reader->ToTable().ValueOrDie();
  std::vector<std::string> names;
  for (const auto &chunk : table->GetColumnByName("table_name")->chunks()) {
    const auto &strings = static_cast<const arrow::StringArray &>(*chunk);
    for (int64_t i = 0; i < strings.length(); ++i) names.emplace_back(strings.GetView(i));
  }
  return names;
}

const std::vector<std::optional<std::string>> kCatalogs = {
    "lake", "memory", "lake", "lakehouse", std::nullopt, "my_lake", "myXlake"};

TEST(CatalogPattern, KeepsRowsOfMatchingCatalogs) {
  auto kept = FilterTablesByCatalogPattern(Info(Listing(kCatalogs)), "lake%");
  EXPECT_EQ((std::vector<std::string>{"t0", "t2", "t3"}), Tables(kept));
  EXPECT_EQ(3, kept->rows);
}

TEST(CatalogPattern, UnderscoreIsOneCharacterUnlessEscaped) {
  EXPECT_EQ((std::vector<std::string>{"t5", "t6"}),
            Tables(FilterTablesByCatalogPattern(Info(Listing(kCatalogs)), "my_lake")));
  EXPECT_EQ((std::vector<std::string>{"t5"}),
            Tables(FilterTablesByCatalogPattern(Info(Listing(kCatalogs)), "my\\_lak_")));
}

TEST(CatalogPattern, NullCatalogMatchesNothingAndNoMatchIsEmpty) {
  EXPECT_EQ((std::vector<std::string>{"t0", "t1", "t2", "t3", "t5", "t6"}),
            Tables(FilterTablesByCatalogPattern(Info(Listing(kCatalogs)), "%_")));
  auto none = FilterTablesByCatalogPattern(Info(Listing(kCatalogs)), "nope%");
  EXPECT_TRUE(Tables(none).empty());
  EXPECT_EQ(4, none->schema->num_fields());
}

TEST(CatalogPattern, ReadsEveryBatch) {
  // Two batches: matches in both must survive.
  auto first = Listing({"lake", "memory"});
  auto second = Listing({"memory", "lake"});
  auto both = arrow::ConcatenateTables({first, second}).ValueOrDie();
  auto kept = FilterTablesByCatalogPattern(Info(both), "l%");
  EXPECT_EQ((std::vector<std::string>{"t0", "t1"}), Tables(kept));
  EXPECT_EQ(2, kept->rows);
}

}  // namespace
}  // namespace chargewire_odbc
}  // namespace driver
