#ifndef TPF_EL_INTRUSIVE_LIST_HPP
#define TPF_EL_INTRUSIVE_LIST_HPP

#include "util.hpp"

namespace el {

class intrusive_list_node {
    NONCOPYABLE(intrusive_list_node);
    template <class T>
    friend class intrusive_list;
    // Null for detached list element nodes -- but set to (this, this) for intrusive_list
    // (which is a cyclic doubly linked list).
    intrusive_list_node *prev_ = nullptr;
    intrusive_list_node *next_ = nullptr;

    bool is_properly_detached() const {
        return prev_ == nullptr && next_ == nullptr;
    }

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

    void insert_before(intrusive_list_node *node) {
        tpf_assert(is_properly_detached());
        intrusive_list_node *prev = node->prev_;
        tpf_assert(prev->next_ == node);
        prev->next_ = this;
        node->prev_ = this;
        prev_ = prev;
        next_ = node;
    }


public:
    intrusive_list_node *next() {
        return next_;
    }
    void detach_self() {
        prev_->next_ = next_;
        next_->prev_ = prev_;
        prev_ = nullptr;
        next_ = nullptr;
    }

    bool is_detached() const {
        return prev_ == nullptr;
    }
};

// T* is the element type, T being a subclass of intrusive_list_node.
template <class T>
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
    bool empty() const {
        return prev_ == this;
    }
    void push(T *elem) {
        intrusive_list_node *node = elem;
        tpf_assert(node->is_detached());
        node->insert_before(this);
    }
};

}

#endif  // TPF_EL_INTRUSIVE_LIST_HPP
