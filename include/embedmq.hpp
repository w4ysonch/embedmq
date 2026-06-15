#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "embedmq.h"

namespace embedmq {

/* ---------------------------------------------------------
 * Callback type
 * --------------------------------------------------------- */
using Handler = std::function<void(const void *data, size_t size)>;

/* ---------------------------------------------------------
 * Internal: trampoline — bridges C handler → C++ lambda
 * --------------------------------------------------------- */
namespace detail {

struct HandlerEntry {
    uint32_t uuid;
    Handler  fn;
};

static void trampoline(const void *data, size_t size, void *ctx)
{
    auto *entry = static_cast<HandlerEntry *>(ctx);
    entry->fn(data, size);
}

} /* namespace detail */

/* ---------------------------------------------------------
 * MQ — RAII wrapper around embedmq_t
 *
 * Usage:
 *
 *   embedmq::MQ q;
 *
 *   q.subscribe("battery.changed", [](const void *data, size_t size) {
 *       const auto *b = static_cast<const battery_t *>(data);
 *       printf("level=%d\n", b->level);
 *   });
 *
 *   q.publish("battery.changed", &info, sizeof(info));
 *
 * --------------------------------------------------------- */
class MQ {
public:
    /* ---- Constructors ---------------------------------- */

    /* Default config */
    MQ() : MQ(nullptr) {}

    /* Custom config */
    explicit MQ(const embedmq_config_t *cfg)
    {
        q_ = embedmq_create(cfg);
        if (!q_)
            throw std::runtime_error("embedmq_create failed");
    }

    /* Static / zero-malloc mode */
    MQ(void *mem, size_t mem_size, const embedmq_config_t *cfg)
    {
        q_ = embedmq_create_static(mem, mem_size, cfg);
        if (!q_)
            throw std::runtime_error("embedmq_create_static failed");
    }

    /* ---- RAII ------------------------------------------ */

    ~MQ()
    {
        if (q_) {
            embedmq_destroy(q_);
            q_ = nullptr;
        }
    }

    /* Non-copyable */
    MQ(const MQ &) = delete;
    MQ &operator=(const MQ &) = delete;

    /* Movable */
    MQ(MQ &&other) noexcept : q_(other.q_), entries_(std::move(other.entries_))
    {
        other.q_ = nullptr;
    }

    MQ &operator=(MQ &&other) noexcept
    {
        if (this != &other) {
            if (q_) embedmq_destroy(q_);
            q_       = other.q_;
            entries_ = std::move(other.entries_);
            other.q_ = nullptr;
        }
        return *this;
    }

    /* ---- subscribe ------------------------------------- */

    /*
     * subscribe — register a lambda / callable for a named event
     *
     * Must be called before any publish() calls.
     * Returns EMBEDMQ_OK on success, negative on error.
     */
    int subscribe(const std::string &name, Handler fn)
    {
        /* Keep the entry alive for the lifetime of this MQ */
        entries_.push_back({ embedmq_uuid(name.c_str()), std::move(fn) });
        auto *entry = &entries_.back();

        return embedmq_register(q_, name.c_str(),
                                detail::trampoline,
                                static_cast<void *>(entry));
    }

    /* ---- publish --------------------------------------- */

    /*
     * publish — enqueue an event by name (non-blocking, thread-safe)
     */
    int publish(const std::string &name,
                const void *data = nullptr, size_t size = 0)
    {
        return embedmq_post(q_, name.c_str(), data, size);
    }

    /*
     * publish_id — enqueue by UUID (hot-path variant)
     *
     * Cache the UUID with uuid() once, then use this on the hot path.
     */
    int publish_id(uint32_t uuid,
                   const void *data = nullptr, size_t size = 0)
    {
        return embedmq_post_id(q_, uuid, data, size);
    }

    /* ---- utilities ------------------------------------ */

    /*
     * uuid — compute UUID for a name (stateless, same as embedmq_uuid())
     */
    static uint32_t uuid(const std::string &name)
    {
        return embedmq_uuid(name.c_str());
    }

    /*
     * poll — manually dispatch pending messages (no-OS mode)
     */
    int poll() { return embedmq_poll(q_); }

    /*
     * native — access the underlying C handle if needed
     */
    embedmq_t *native() const { return q_; }

private:
    embedmq_t                           *q_ = nullptr;
    std::vector<detail::HandlerEntry>    entries_;
};

} /* namespace embedmq */
