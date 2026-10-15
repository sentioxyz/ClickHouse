#include <gtest/gtest.h>

#include <cstdlib>
#include <fcntl.h>
#include <Formats/FormatSettings.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/WriteHelpers.h>

#include <base/sanitizer_options.h>

#include <Common/ThreadPool.h>
#include <Common/scope_guard_safe.h>
#include <IO/SharedThreadPools.h>
#include <Common/tests/gtest_global_context.h>

#include <Poco/ThreadPool.h>

/// Parameter printers may include process-local pointers. Record registration metadata in the same process as the test report,
/// instead of weakening the comparison by stripping addresses or dropping parameter values.
class RuntimeCatalogListener : public testing::EmptyTestEventListener
{
public:
    explicit RuntimeCatalogListener(const char * path_) : path(path_) {}

    void OnTestProgramStart(const testing::UnitTest & unit) override
    {
        DB::WriteBufferFromFile out(path, 4096, O_WRONLY | O_CREAT | O_EXCL);
        const DB::FormatSettings settings;
        size_t index = 0;
        for (int suite_index = 0; suite_index < unit.total_test_suite_count(); ++suite_index)
        {
            const auto * suite = unit.GetTestSuite(suite_index);
            for (int test_index = 0; test_index < suite->total_test_count(); ++test_index)
            {
                const auto * test = suite->GetTestInfo(test_index);
                auto nullable = [&](const char * value)
                {
                    if (value)
                        DB::writeJSONString(value, out, settings);
                    else
                        DB::writeCString("null", out);
                };
                DB::writeCString("{\"index\":", out);
                DB::writeIntText(index++, out);
                DB::writeCString(",\"suite\":", out);
                DB::writeJSONString(suite->name(), out, settings);
                DB::writeCString(",\"test\":", out);
                DB::writeJSONString(test->name(), out, settings);
                DB::writeCString(",\"file\":", out);
                nullable(test->file());
                DB::writeCString(",\"line\":", out);
                DB::writeIntText(test->line(), out);
                DB::writeCString(",\"type_param\":", out);
                nullable(test->type_param());
                DB::writeCString(",\"value_param\":", out);
                nullable(test->value_param());
                DB::writeCString(",\"selected\":", out);
                DB::writeChar(test->should_run() ? '1' : '0', out);
                DB::writeCString("}\n", out);
            }
        }
        out.finalize();
    }

private:
    std::string path;
};

class ContextEnvironment : public testing::Environment
{
public:
    void SetUp() override { getContext(); }
    void TearDown() override { getMutableContext().destroy(); }
};

int main(int argc, char ** argv)
{
    /// Join global-pool threads before the statics they may have accessed are destroyed.
    /// That way, accesses happen-before destruction.
    SCOPE_EXIT_SAFE({
        Poco::ThreadPool::defaultPool().stopAll();
        DB::StaticThreadPool::shutdownAll();
        GlobalThreadPool::shutdown();
    });

    testing::InitGoogleTest(&argc, argv);

    if (const char * runtime_catalog = std::getenv("CLICKHOUSE_GTEST_RUNTIME_CATALOG"))
        testing::UnitTest::GetInstance()->listeners().Append(new RuntimeCatalogListener(runtime_catalog));

    auto & options = getTestCommandLineOptions();
    options.argc = argc;
    options.argv = argv;

    testing::AddGlobalTestEnvironment(new ContextEnvironment);

    return RUN_ALL_TESTS();
}
