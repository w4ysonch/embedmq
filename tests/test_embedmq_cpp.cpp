#include <cassert>
#include <cstdio>
#include <atomic>
#include "embedmq.hpp"

static void test_subscribe_and_publish()
{
    std::atomic<int> count{0};

    {
        embedmq::MQ q;
        q.subscribe("evt.a", [&count](const void *, size_t) { count++; });

        q.publish("evt.a");
        q.publish("evt.a");
        q.publish("evt.a");
    }

    assert(count == 3);
    printf("  [PASS] test_subscribe_and_publish\n");
}

static void test_lambda_capture()
{
    std::atomic<int> result{0};

    {
        embedmq::MQ q;
        q.subscribe("int.evt", [&result](const void *data, size_t size) {
            assert(size == sizeof(int));
            result = *static_cast<const int *>(data);
        });

        int val = 42;
        q.publish("int.evt", &val, sizeof(val));
    } /* destructor drains queue before returning */

    assert(result == 42);
    printf("  [PASS] test_lambda_capture\n");
}

static void test_publish_id_hot_path()
{
    std::atomic<int> count{0};

    {
        embedmq::MQ q;
        q.subscribe("hot.evt", [&count](const void *, size_t) { count++; });

        uint32_t uuid = embedmq::MQ::uuid("hot.evt");
        for (int i = 0; i < 100; i++)
            q.publish_id(uuid);
    }

    assert(count == 100);
    printf("  [PASS] test_publish_id_hot_path\n");
}

static void test_raii_destroy()
{
    std::atomic<int> count{0};
    {
        embedmq::MQ q;
        q.subscribe("raii.evt", [&count](const void *, size_t) { count++; });
        q.publish("raii.evt");
    } /* destructor drains queue before returning */
    assert(count == 1);
    printf("  [PASS] test_raii_destroy\n");
}

static void test_move()
{
    std::atomic<int> count{0};

    {
        embedmq::MQ q1;
        q1.subscribe("move.evt", [&count](const void *, size_t) { count++; });

        embedmq::MQ q2 = std::move(q1);
        q2.publish("move.evt");
    }

    assert(count == 1);
    printf("  [PASS] test_move\n");
}

int main()
{
    printf("embedmq C++ wrapper test suite\n");
    test_subscribe_and_publish();
    test_lambda_capture();
    test_publish_id_hot_path();
    test_raii_destroy();
    test_move();
    printf("All C++ tests passed.\n");
    return 0;
}
