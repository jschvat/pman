#include "pman/async/task.hpp"
#include "pman/async/event_loop.hpp"

namespace pman::async {
namespace detail {

void deferTaskDeletion(void* task_ptr, void(*deleter)(void*)) {
    EventLoop* loop = EventLoop::current();
    if (loop) {
        // Post cleanup to event loop to defer destruction
        loop->post([task_ptr, deleter]() {
            deleter(task_ptr);
        });
    } else {
        // No event loop - shouldn't happen but handle gracefully
        deleter(task_ptr);
    }
}

} // namespace detail
} // namespace pman::async
