// the position index shared by WOOT, RGA, Logoot and LSEQ: OrderedSequence<Element>.
//
// it is a "treap": a binary tree that keeps the elements in document order, where every
// node also gets a random priority and parents always have a higher priority than their
// children. the random priorities keep the tree balanced on average.
//
// every node remembers how many nodes are below it, counted twice: all of them
// (including hidden ones, e.g. deleted elements kept as tombstones) and only the visible
// ones. with those counts we can, in about log(n) steps,
//   * find the k-th visible element (local edits address elements by position),
//   * find the index of a node (WOOT compares positions of characters),
//   * insert next to a known node and hide/show a node.
// giving the four list-like algorithms the same index keeps the comparison fair: their
// measured differences come from the algorithms themselves, not from one of them using
// a slow linked list.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace crdt {

template <class Element>
class OrderedSequence {
public:
    struct Node {
        Element value;
        Node* left = nullptr;
        Node* right = nullptr;
        Node* parent = nullptr;
        uint32_t priority;
        uint32_t subtree_size = 1; // nodes in subtree, including this one
        uint32_t subtree_visible; // visible nodes in subtree, including this one
        bool visible;

        Node(Element element, bool is_visible, uint32_t node_priority)
            : value(std::move(element)),
              priority(node_priority),
              subtree_visible(is_visible ? 1u : 0u),
              visible(is_visible) {}
    };

    explicit OrderedSequence(uint64_t seed = 0x9E3779B97F4A7C15ull) : random_state_(seed | 1) {}
    ~OrderedSequence() { clear(); }
    OrderedSequence(const OrderedSequence&) = delete;
    OrderedSequence& operator=(const OrderedSequence&) = delete;

    size_t size() const { 
        return root_ ? root_->subtree_size : 0;
    }

    size_t visible_size() const { 
        return root_ ? root_->subtree_visible : 0;
    }

    Node* first() const { 
        return root_ ? leftmost(root_) : nullptr; 
    }

    Node* last() const { 
        return root_ ? rightmost(root_) : nullptr;
    }

    // the node right after `node` in document order
    static Node* next(const Node* node) {
        if (node->right) 
            return leftmost(node->right);

        // climb up until we come from a left child -> that parent is next
        const Node* child = node;
        Node* ancestor = node->parent;
        while (ancestor && ancestor->right == child) {
            child = ancestor;
            ancestor = ancestor->parent;
        }

        return ancestor;
    }

    // inserts right after `position`
    Node* insert_after(Node* position, Element element, bool visible) {
        Node* node = new Node(std::move(element), visible, next_priority());

        if (!root_) {
            root_ = node;
            return node;
        }

        // create new node as a leaf in the right spot, rotate by priority
        if (!position) 
            attach(leftmost(root_), node, true);
        else if (!position->right) 
            attach(position, node, false);
        else 
            attach(leftmost(position->right), node, true);

        bubble_up(node);
        return node;
    }

    // insert before `position` 
    Node* insert_before(Node* position, Element element, bool visible) {
        Node* node = new Node(std::move(element), visible, next_priority());
        if (!root_) {
            root_ = node;
            return node;
        }

        if (!position) 
            attach(rightmost(root_), node, false);
        else if (!position->left) 
            attach(position, node, true);
        else 
            attach(rightmost(position->left), node, false);

        bubble_up(node);
        return node;
    }

    void erase(Node* node) {
      // rotate node down until it is a leaf  (lift higher priority children)
      while (node->left || node->right) {
        Node* child;
        if (!node->right)
          child = node->left;
        else if (!node->left)
          child = node->right;
        else
          child = (node->left->priority > node->right->priority) ? node->left
                                                                 : node->right;

        rotate_up(child);
      }

        Node* parent = node->parent;
        if (!parent) 
            root_ = nullptr;
        else if (parent->left == node) 
            parent->left = nullptr;
        else
             parent->right = nullptr;

        for (Node* ancestor = parent; ancestor; ancestor = ancestor->parent) {
            ancestor->subtree_size--;
            if (node->visible) 
                ancestor->subtree_visible--;
        }

        delete node;
    }

    void set_visible(Node* node, bool is_visible) {
        if (node->visible == is_visible) 
            return;

        node->visible = is_visible;
        for (Node* ancestor = node; ancestor; ancestor = ancestor->parent) {
            if (is_visible) 
                ancestor->subtree_visible++;
            else 
                ancestor->subtree_visible--;
        }
    }

    // visible node with `rank` visible nodes before it
    Node* at_visible(size_t rank) const {
        Node* node = root_;
        while (node) {
            size_t left_visible = node->left ? node->left->subtree_visible : 0;
            if (rank < left_visible) {
                node = node->left;
                continue;
            }

            rank -= left_visible;

            if (node->visible) {
                if (rank == 0) 
                    return node;

                rank -= 1;
            }

            node = node->right;
        }

        return nullptr;
    }

    // how many nodes come before `node`
    size_t index_of(const Node* node) const {
        size_t index = node->left ? node->left->subtree_size : 0;
        for (const Node *child = node, *ancestor = node->parent; ancestor; child = ancestor, ancestor = ancestor->parent) {
            if (ancestor->right == child) 
                index += (ancestor->left ? ancestor->left->subtree_size : 0) + 1;
        }

        return index;
    }

    // binary search in a sequence kept sorted by key
    // returns the first node for which `is_before_key(value)` is false
    template <class IsBeforeKey>
    Node* lower_bound(IsBeforeKey is_before_key) const {
        Node* node = root_;
        Node* result = nullptr;
        while (node) {
            if (is_before_key(node->value)) {
                node = node->right;
            } else {
                result = node;
                node = node->left;
            }
        }

        return result;
    }

    template <class Visitor>
    void for_each(Visitor visit) const {
        for (Node* node = first(); node; node = next(node)) visit(*node);
    }

    // replaces all content with `items` (value, visible) in a single pass.
    // goes through the times left to right and keeps the right edge of the tree
    // new nodes pop lower priority nodes of the edge, then those go in the left subtree
    // basically uilding a treap in time proportional to number of items.
    void build(std::vector<std::pair<Element, bool>>&& items) {
        clear();

        std::vector<Node*> right_edge;
        right_edge.reserve(64);
        for (auto& item : items) {
            Node* node = new Node(std::move(item.first), item.second, next_priority());

            Node* last_popped = nullptr;
            while (!right_edge.empty() && right_edge.back()->priority < node->priority) {
                last_popped = right_edge.back();
                right_edge.pop_back();
            }

            node->left = last_popped;
            if (last_popped) 
                last_popped->parent = node;

            if (!right_edge.empty()) {
                right_edge.back()->right = node;
                node->parent = right_edge.back();
            }

            right_edge.push_back(node);
        }

        root_ = right_edge.empty() ? nullptr : right_edge.front();
        recompute_counts(root_);
    }

    void clear() {
        delete_subtree(root_);
        root_ = nullptr;
    }

    size_t height() const { return height_of(root_); }

    // checks parent links, priorities and counters
    // problem -> return false and describes it
    bool check(std::string* error) const {
        if (root_ && root_->parent) 
            return fail(error, "root has parent");

        return check_subtree(root_, error);
    }

    static constexpr size_t node_size() { return sizeof(Node); }

private:
    Node* root_ = nullptr;
    uint64_t random_state_;

    // pseudo-random priority (xorshift64*)
    uint32_t next_priority() {
        random_state_ ^= random_state_ >> 12;
        random_state_ ^= random_state_ << 25;
        random_state_ ^= random_state_ >> 27;
        return static_cast<uint32_t>((random_state_ * 2685821657736338717ull) >> 32);
    }

    static Node* leftmost(Node* node) {
        while (node->left) 
            node = node->left;

        return node;
    }

    static Node* rightmost(Node* node) {
        while (node->right) 
            node = node->right;

        return node;
    }

    static void attach(Node* parent, Node* child, bool as_left) {
        if (as_left) 
            parent->left = child;
        else 
            parent->right = child;

        child->parent = parent;
    }

    // new leaf got added - count it in every ancestor, after that
    // rotate it up until priority is no longer bigger than the parents
    void bubble_up(Node* node) {
        for (Node* ancestor = node->parent; ancestor; ancestor = ancestor->parent) {
            ancestor->subtree_size++;
            if (node->visible)
                ancestor->subtree_visible++;
        }

        while (node->parent && node->parent->priority < node->priority) 
            rotate_up(node);
    }

    static void update_counts(Node* node) {
        node->subtree_size = 1 + (node->left ? node->left->subtree_size : 0) + (node->right ? node->right->subtree_size : 0);
        node->subtree_visible = (node->visible ? 1u : 0u) + (node->left ? node->left->subtree_visible : 0) +
                                (node->right ? node->right->subtree_visible : 0);
    }

    // swaps `node` with its parent while keeping the document order the same
    void rotate_up(Node* node) {
        Node* parent = node->parent;
        Node* grandparent = parent->parent;

        if (parent->left == node) {
            parent->left = node->right;

            if (node->right) 
                node->right->parent = parent;

            node->right = parent;
        } else {
            parent->right = node->left;

            if (node->left)
                node->left->parent = parent;

            node->left = parent;
        }

        parent->parent = node;
        node->parent = grandparent;

        if (!grandparent) 
            root_ = node;
        else if (grandparent->left == parent)
            grandparent->left = node;
        else
            grandparent->right = node;

        update_counts(parent);
        update_counts(node);
    }

    // fills counters of new tree
    // children must be counted before parents, therefore
    // list the nodes top-down and then update in reverse order
    static void recompute_counts(Node* root) {
        if (!root) return;

        std::vector<Node*> top_down;
        std::vector<Node*> pending{root};
        while (!pending.empty()) {
            Node* node = pending.back();
            pending.pop_back();
            top_down.push_back(node);

            if (node->left)
                pending.push_back(node->left);

            if (node->right) 
                pending.push_back(node->right);
        }

        for (auto node = top_down.rbegin(); node != top_down.rend(); ++node)
            update_counts(*node);
    }

    static void delete_subtree(Node* node) {
        if (!node) 
            return;

        delete_subtree(node->left);
        delete_subtree(node->right);

        delete node;
    }

    static size_t height_of(const Node* node) {
        if (!node)
            return 0;
        size_t left = height_of(node->left);
        size_t right = height_of(node->right);

        return 1 + (left > right ? left : right);
    }

    static bool check_subtree(const Node* node, std::string* error) {
        if (!node) 
            return true;

        for (const Node* child : {node->left, node->right}) {
            if (!child) 
                continue;

            if (child->parent != node)
                return fail(error, "bad parent pointer");

            if (child->priority > node->priority)
                return fail(error, "heap order violated");
        }

        if (!check_subtree(node->left, error) || !check_subtree(node->right, error)) 
            return false;

        size_t expected_size = 1;
        size_t expected_visible = node->visible ? 1 : 0;

        for (const Node* child : {node->left, node->right}) {
            if (!child) 
                continue;

            expected_size += child->subtree_size;
            expected_visible += child->subtree_visible;
        }

        if (expected_size != node->subtree_size) 
            return fail(error, "subtree_size counter mismatch");

        if (expected_visible != node->subtree_visible) 
            return fail(error, "subtree_visible counter mismatch");

        return true;
    }

    static bool fail(std::string* error, const char* message) {
        if (error) 
            *error = message;

        return false;
    }
};

// rough heap size of a std::unordered_map
// bucket array + 1 node per entry
template <class Map>
size_t unordered_map_bytes(const Map& map) {
    return map.bucket_count() * sizeof(void*) + map.size() * (sizeof(typename Map::value_type) + sizeof(void*));
}

}  // namespace crdt
