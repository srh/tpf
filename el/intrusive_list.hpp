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
    ~intrusive_list_node() {
        // Unclear if we should assert or if we should just detach self upon destruction.
        tpf_assert(is_properly_detached());
    }
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
        tpf_assert(next_ != nullptr);
        return next_;
    }
    void detach() {
        tpf_assert(!is_detached());
        tpf_assert(next_ != this);
        next_->prev_ = prev_;
        prev_->next_ = next_;
        next_ = nullptr;
        prev_ = nullptr;
    }

    bool is_detached() const {
        tpf_assert((next_ == nullptr) == (prev_ == nullptr));
        return next_ == nullptr;
    }
};

// A doubly linked intrusive linked list.  Is cyclic.
// T* is the element type, T being a subclass of intrusive_list_node.
template <class T>
class intrusive_list : private intrusive_list_node {
public:
    NONCOPYABLE(intrusive_list);
    intrusive_list() : intrusive_list_node{} {
        next_ = this;
        prev_ = this;
    }
    ~intrusive_list() {
        tpf_assert(next_ == this);
        tpf_assert(prev_ == this);
        // set to nullptr so that ~intrusive_list_node superclass can assert the node is detached.
        next_ = nullptr;
        prev_ = nullptr;
    }
    intrusive_list(intrusive_list&& other) : intrusive_list_node() {
        // Careful: This is "slick" in how it handles the empty list case.
        intrusive_list_node *other_next = other.next_;
        intrusive_list_node *other_prev = other.prev_;
        other_next->prev_ = this;
        other_prev->next_ = this;
        // Empty list case: Here other.next_ and other.prev_ point at this.
        next_ = other.next_;
        prev_ = other.prev_;
        // Empty list case: next_ and prev_ point at this (because other.next_ got updated first).

        // Unlike intrusive_list_node(intrusive_list_node&&), we reset other to point at self.
        other.next_ = &other;
        other.prev_ = &other;

        // TODO: Remove
        assert_shallow();
    }
    void operator=(intrusive_list&& other) = delete;

    void swap(intrusive_list& other) {
        intrusive_list_node *other_next = other.next_;
        intrusive_list_node *other_prev = other.prev_;
        intrusive_list_node *og_next = next_;
        intrusive_list_node *og_prev = prev_;
        other_next->prev_ = this;
        other_prev->next_ = this;
        // other is empty case:  here other.next_ and other.prev_ point at this.
        next_ = other.next_;
        prev_ = other.prev_;
        // other is empty case:  next_ and prev_ point at this (because other.next_ got updated first).

        // TODO: Not a fan of this condition, but also not a fan of the pointer-aliasing
        // slickness (which may have its own CPU perf penalty) in the move ctor either.
        if (og_next == this) {
            other.next_ = &other;
            other.prev_ = &other;
        } else {
            other.next_ = og_next;
            other.prev_ = og_prev;
            og_next->prev_ = &other;
            og_prev->next_ = &other;
        }

        // TODO: Remove.
        assert_shallow();
        other.assert_shallow();
    }

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

    void assert_shallow() {
        tpf_assert(next_ != nullptr);
        tpf_assert(prev_ != nullptr);
        intrusive_list_node *selph = self();
        tpf_assert(selph != nullptr);
        tpf_assert(selph == this);
        tpf_assert((next_ == selph) == (prev_ == selph));
        tpf_assert(next_->prev_ == selph);
        tpf_assert(prev_->next_ == selph);
    }

    void assert_deep() {
        assert_shallow();
        intrusive_list_node *node = head();
        tpf_assert(node == next_);
        intrusive_list_node *selph = self();
        tpf_assert(selph == this);
        tpf_assert(selph != nullptr);
        while (node != selph) {
            tpf_assert(node != nullptr);
            tpf_assert(node->next_ != nullptr);
            tpf_assert(node == node->next_->prev_);
            node = node->next_;
        }
    }
};

}

#endif  // TPF_EL_INTRUSIVE_LIST_HPP
