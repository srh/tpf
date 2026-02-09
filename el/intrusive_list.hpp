#ifndef TPF_EL_INTRUSIVE_LIST_HPP
#define TPF_EL_INTRUSIVE_LIST_HPP

#include "util.hpp"

namespace el {

// A node in a cyclic doubly linked list.
class intrusive_list_node {
    NONCOPYABLE(intrusive_list_node);
    template <class T>
    friend class intrusive_list;

    // A "detached" node has its pointers set to nullptr.  Another choice might be to make
    // it point at self when detached.
    intrusive_list_node *next_ = nullptr;
    intrusive_list_node *prev_ = nullptr;

    bool is_properly_detached() const {
        return next_ == nullptr && prev_ == nullptr;
    }

protected:
    intrusive_list_node() = default;
    intrusive_list_node(intrusive_list_node&& other) noexcept {
        if (other.next_ != nullptr) {
            other.next_->prev_ = this;
            other.prev_->next_ = this;
            next_ = other.next_;
            prev_ = other.prev_;
            // Note that intrusive_list(intrusive_list&&) which avoids this ctor has
            // different behavior.
            other.next_ = nullptr;
            other.prev_ = nullptr;
        }
    }
    void operator=(intrusive_list_node&) = delete;

    void insert_before(intrusive_list_node *node) {
        tpf_assert(is_properly_detached());
        intrusive_list_node *prev = node->prev_;
        tpf_assert(prev->next_ == node);
        prev->next_ = this;
        node->prev_ = this;
        next_ = node;
        prev_ = prev;
    }

public:
    intrusive_list_node *next() {
        return next_;
    }
    void detach() {
        tpf_assert(!is_detached());
        next_->prev_ = prev_;
        prev_->next_ = next_;
        next_ = nullptr;
        prev_ = nullptr;
    }

    bool is_detached() const {
        return next_ == nullptr;
    }
};

// A doubly linked intrusive linked list.  Is cyclic.
// T* is the element type, T being a subclass of intrusive_list_node.
template <class T>
class intrusive_list : private intrusive_list_node {
public:
    using element_pointer_type = T *;
    NONCOPYABLE(intrusive_list);
    intrusive_list() : intrusive_list_node{} {
        next_ = this;
        prev_ = this;
    }
    intrusive_list(intrusive_list&& other) : intrusive_list_node() {
        // Careful: This is "slick" in how it handles the empty list case.
        other.next_->prev_ = this;
        other.prev_->next_ = this;
        // Empty list case: Here other.next_ and other.prev_ point at this.

        next_ = other.next_;
        prev_ = other.prev_;
        // Empty list case: next_ and prev_ point at this (because other.next_ got updated first).

        // Unlike intrusive_list_node(intrusive_list_node&&), we reset other to point at self.
        other.next_ = &other;
        other.prev_ = &other;
    }
    void operator=(intrusive_list&& other) = delete;

    intrusive_list_node *head() {
        return next_;
    }
    intrusive_list_node *self() {
        return this;
    }
    bool empty() const {
        return next_ == this;
    }
    void push(T *elem) {
        intrusive_list_node *node = elem;
        tpf_assert(node->is_detached());
        node->insert_before(this);
    }
};

}

#endif  // TPF_EL_INTRUSIVE_LIST_HPP
