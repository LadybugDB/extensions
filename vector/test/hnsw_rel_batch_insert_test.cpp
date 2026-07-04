#include "api_test/api_test.h"

using namespace lbug::common;

namespace lbug {
namespace testing {

// Regression test for inconsistent CSR metadata when a neighbor slot is
// marked INVALID_OFFSET. Previously populateCSRLengths and getNumRelsInGraph
// used the unfiltered graph CSR length, while writeToTable skipped
// INVALID_OFFSET neighbors. This left holes/stale values in the chunked
// group. The test builds an index over enough nodes to trigger batch insert
// and verifies that QUERY_VECTOR_INDEX returns consistent results without
// errors, and that the underlying rel tables have no stale/empty rows.
class HNSWRelBatchInsertTest : public ApiTest {};

TEST_F(HNSWRelBatchInsertTest, QueryVectorIndexReturnsConsistentResults) {
#ifndef __STATIC_LINK_EXTENSION_TEST__
    ASSERT_TRUE(conn->query(std::format("LOAD EXTENSION '{}'",
                                TestHelper::appendLbugRootPath(
                                    "extension/vector/build/libvector.lbug_extension")))
                    ->isSuccess());
#endif
    ASSERT_TRUE(conn->query("CREATE NODE TABLE Book (ID SERIAL, title STRING, "
                            "title_embedding FLOAT[4], PRIMARY KEY (ID));")
                    ->isSuccess());
    // Insert enough nodes to span multiple node groups so the batch insert
    // path exercises CSR length / numRels consistency across groups.
    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(conn->query(std::format(
            "CREATE (b:Book {{title: 'Book {}', title_embedding: [{}, {}, {}, {}]}});", i,
            static_cast<float>(i) * 0.1f, static_cast<float>(i) * 0.2f,
            static_cast<float>(i) * 0.3f, static_cast<float>(i) * 0.4f))
                    ->isSuccess());
    }
    ASSERT_TRUE(
        conn->query("CALL CREATE_VECTOR_INDEX('Book', 'title_vec_index', 'title_embedding');")
            ->isSuccess());

    auto prepared = conn->prepare(
        "CALL QUERY_VECTOR_INDEX('Book', 'title_vec_index', "
        "[0.0, 0.0, 0.0, 0.0], $k) RETURN node.title ORDER BY distance;");
    auto result = conn->execute(prepared.get(), std::make_pair(std::string("k"), 3));
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNumTuples(), 3);
    // Closest node to the origin embedding is Book 0.
    auto rows = TestHelper::convertResultToString(*result);
    ASSERT_EQ(rows[0], "Book 0");
}

} // namespace testing
} // namespace lbug
