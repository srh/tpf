#ifndef TPF_EL_INTRUSIVE_LIST_HPP
#define TPF_EL_INTRUSIVE_LIST_HPP

#include "util.hpp"

namespace el {

class intrusive_list_node {
    NONCOPYABLE(intrusive_list_node);
    friend class intrusive_list;
    // Null for detached list element nodes -- but set to (this, this) for intrusive_list
    // (which is a cyclic doubly linked list).
    intrusive_list_node *prev_ = nullptr;
    intrusive_list_node *next_ = nullptr;

protected:
    intrusive_list_node() = default;
    intrusive_list_node(intrusive_list_node&& other) noexcept {
        // This uses aliasing to handle empty list case...
        if (other.next_ != nullptr) {
            other.next_->prev_ = this;
            other.prev_->next_ = this;
            next_ = other.next_;
            prev_ = other.prev_;
        }
    }
    void operator=(intrusive_list_node&) = delete;
};

class intrusive_list : private intrusive_list_node {
public:
    NONCOPYABLE(intrusive_list);
    intrusive_list() : intrusive_list_node{} {
        prev_ = this;
        next_ = this;
    }
    intrusive_list(intrusive_list&& other) {
        other.next_->prev_ = this;
        other.prev_->next_ = this;
        next_ = other.next_;
        prev_ = other.prev_;
    }
    void operator=(intrusive_list&& other) = delete;

    intrusive_list_node *head() {
        return next_;
    }
    intrusive_list_node *self() {
        return this;
    }
    static intrusive_list_node *next(intrusive_list_node *node) {
        return node->next_;
    }
};

}

#endif  // TPF_EL_INTRUSIVE_LIST_HPP
