#include <not_implemented.h>
#include "../include/allocator_sorted_list.h"
#include <cstring>
#include <stdexcept>

allocator_sorted_list::~allocator_sorted_list()
{
    if (_trusted_memory == nullptr)
    
    {
        return;
    }

    auto* meta = meta_of(_trusted_memory);

    meta->mtx.~mutex();

    if (meta->parent_allocator != nullptr)
    {
        meta->parent_allocator->deallocate(
            _trusted_memory,
            meta->total_size,
            alignof(std::max_align_t)
        );
    }
    else
    {
        ::operator delete(_trusted_memory);
    }

    _trusted_memory = nullptr;
}

allocator_sorted_list::allocator_sorted_list(
    allocator_sorted_list &&other) noexcept
{
    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;
}

allocator_sorted_list &allocator_sorted_list::operator=(
    allocator_sorted_list &&other) noexcept
{
    if (this != &other)
    {
        this->~allocator_sorted_list();

        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }

    return *this;
}

allocator_sorted_list::allocator_sorted_list(
        size_t space_size,
        std::pmr::memory_resource *parent_allocator,
        allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    size_t aligned_space = (space_size + alignof(std::max_align_t) - 1) & ~(alignof(std::max_align_t) - 1);
    size_t total_size = allocator_metadata_size + aligned_space;

    void* memory = nullptr;

    if (parent_allocator != nullptr)
    {
        memory = parent_allocator->allocate(
            total_size,
            alignof(std::max_align_t)
        );
    }
    else
    {
        memory = ::operator new(total_size, std::nothrow);
    }

    if (memory == nullptr)
    {
        throw std::bad_alloc();
    }

    _trusted_memory = memory;

    auto* meta = new (_trusted_memory) allocator_meta{
        parent_allocator,
        allocate_fit_mode,
        total_size,
        {},
        nullptr
    };

    void* first_block_ptr = static_cast<char*>(_trusted_memory) + allocator_metadata_size;
    meta->first_free_block = first_block_ptr;

    auto* block = block_of(first_block_ptr);
    block->size = aligned_space;
    block->next = nullptr;
}

[[nodiscard]] void *allocator_sorted_list::do_allocate_sm(
    size_t size)
{
    if (_trusted_memory == nullptr)
    {
        throw std::logic_error("allocator_sorted_list: uninitialized memory");
    }

    if (size == 0)
    {
        return nullptr;
    }

    auto* meta = meta_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);

    size_t required_block_size = (size + block_metadata_size + alignof(std::max_align_t) - 1) & ~(alignof(std::max_align_t) - 1);

    search_res result;

    switch (meta->mode)
    {
        case allocator_with_fit_mode::fit_mode::first_fit:
            result = first_fit(required_block_size);
            break;

        case allocator_with_fit_mode::fit_mode::the_best_fit:
            result = best_fit(required_block_size);
            break;

        case allocator_with_fit_mode::fit_mode::the_worst_fit:
            result = worst_fit(required_block_size);
            break;
    }

    if (result.target == nullptr)
    {
        throw std::bad_alloc();
    }

    auto* target_block = block_of(result.target);
    auto* prev_block = result.prev ? block_of(result.prev) : nullptr;

    size_t original_size = target_block->size;

    if (original_size >= required_block_size + block_metadata_size + alignof(std::max_align_t))
    {
        size_t remaining_size = original_size - required_block_size;

        target_block->size = remaining_size;

        auto* allocated_block = reinterpret_cast<block_header*>(reinterpret_cast<char*>(target_block) + remaining_size);

        allocated_block->size = required_block_size;

        target_block = allocated_block;
    }
    else
    {
        if (prev_block != nullptr)
        {
            prev_block->next = target_block->next;
        }
        else
        {
            meta->first_free_block = target_block->next;
        }
    }

    target_block->next = nullptr;

    return block_to_user(target_block);
}

allocator_sorted_list::search_res allocator_sorted_list::first_fit(size_t size) const
{
    auto* meta = meta_of(_trusted_memory);
    void* curr = meta->first_free_block;
    void* prev = nullptr;

    while (curr != nullptr)
    {
        if (block_of(curr)->size >= size)
        {
            return { curr, prev };
        }
        prev = curr;
        curr = block_of(curr)->next;
    }
    return { nullptr, nullptr };
}

allocator_sorted_list::search_res allocator_sorted_list::best_fit(size_t size) const
{
    auto* meta = meta_of(_trusted_memory);
    void* curr = meta->first_free_block;
    void* prev = nullptr;

    void* best_found = nullptr;
    void* best_prev = nullptr;
    size_t min_diff = SIZE_MAX;

    while (curr != nullptr)
    {
        size_t curr_size = block_of(curr)->size;
        if (curr_size >= size)
        {
            size_t diff = curr_size - size;
            if (diff < min_diff)
            {
                min_diff = diff;
                best_found = curr;
                best_prev = prev;
            }
        }
        prev = curr;
        curr = block_of(curr)->next;
    }
    return { best_found, best_prev };
}

allocator_sorted_list::search_res allocator_sorted_list::worst_fit(size_t size) const
{
    auto* meta = meta_of(_trusted_memory);
    void* curr = meta->first_free_block;
    void* prev = nullptr;

    void* worst_found = nullptr;
    void* worst_prev = nullptr;
    size_t max_diff = 0;
    bool found_at_least_one = false;

    while (curr != nullptr)
    {
        size_t curr_size = block_of(curr)->size;
        if (curr_size >= size)
        {
            size_t diff = curr_size - size;
            if (!found_at_least_one || diff > max_diff)
            {
                max_diff = diff;
                worst_found = curr;
                worst_prev = prev;
                found_at_least_one = true;
            }
        }
        prev = curr;
        curr = block_of(curr)->next;
    }
    return { worst_found, worst_prev };
}

allocator_sorted_list::allocator_sorted_list(const allocator_sorted_list &other)
{
    if (other._trusted_memory == nullptr)
    {
        _trusted_memory = nullptr;
        return;
    }

    auto* other_meta = meta_of(other._trusted_memory);

    std::lock_guard<std::mutex> lock(other_meta->mtx);

    void* memory = nullptr;

    if (other_meta->parent_allocator != nullptr)
    {
        memory = other_meta->parent_allocator->allocate(
            other_meta->total_size,
            alignof(std::max_align_t)
        );
    }
    else
    {
        memory = ::operator new(other_meta->total_size, std::nothrow);
    }

    if (memory == nullptr)
    {
        throw std::bad_alloc();
    }

    _trusted_memory = memory;

    std::memcpy(_trusted_memory, other._trusted_memory, other_meta->total_size);

    auto* new_meta = meta_of(_trusted_memory);
    new (&new_meta->mtx) std::mutex();

    ptrdiff_t offset = static_cast<char*>(_trusted_memory) - static_cast<char*>(other._trusted_memory);

    if (new_meta->first_free_block != nullptr)
    {
        new_meta->first_free_block = static_cast<char*>(new_meta->first_free_block) + offset;

        void* curr = new_meta->first_free_block;
        while (curr != nullptr)
        {
            auto* node = block_of(curr);
            if (node->next != nullptr)
            {
                node->next = static_cast<char*>(node->next) + offset;
            }
            curr = node->next;
        }
    }
}

allocator_sorted_list &allocator_sorted_list::operator=(const allocator_sorted_list &other)
{
    if (this == &other)
    {
        return *this;
    }

    this->~allocator_sorted_list();

    if (other._trusted_memory == nullptr)
    {
        _trusted_memory = nullptr;
        return *this;
    }

    auto* other_meta = meta_of(other._trusted_memory);

    std::lock_guard<std::mutex> lock(other_meta->mtx);

    void* memory = nullptr;

    if (other_meta->parent_allocator != nullptr)
    {
        memory = other_meta->parent_allocator->allocate(
            other_meta->total_size,
            alignof(std::max_align_t)
        );
    }
    else
    {
        memory = ::operator new(other_meta->total_size, std::nothrow);
    }

    if (memory == nullptr)
    {
        throw std::bad_alloc();
    }

    _trusted_memory = memory;

    std::memcpy(_trusted_memory, other._trusted_memory, other_meta->total_size);

    auto* new_meta = meta_of(_trusted_memory);
    new (&new_meta->mtx) std::mutex();

    ptrdiff_t offset = static_cast<char*>(_trusted_memory) - static_cast<char*>(other._trusted_memory);

    if (new_meta->first_free_block != nullptr)
    {
        new_meta->first_free_block = static_cast<char*>(new_meta->first_free_block) + offset;

        void* curr = new_meta->first_free_block;
        while (curr != nullptr)
        {
            auto* node = block_of(curr);
            if (node->next != nullptr)
            {
                node->next = static_cast<char*>(node->next) + offset;
            }
            curr = node->next;
        }
    }

    return *this;
}

bool allocator_sorted_list::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return this == &other;
}

void allocator_sorted_list::do_deallocate_sm(
    void *at)
{
    if (_trusted_memory == nullptr)
    {
        throw std::logic_error("allocator_sorted_list: uninitialized memory");
    }

    if (at == nullptr)
    {
        return;
    }

    auto* meta = meta_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);

    char* target_ptr = static_cast<char*>(at);
    char* start = static_cast<char*>(_trusted_memory) + allocator_metadata_size;
    char* end = static_cast<char*>(_trusted_memory) + meta->total_size;

    if (target_ptr < start || target_ptr >= end)
    {
        throw std::logic_error("allocator_sorted_list: pointer is out of this allocator range");
    }

    auto* returning_block = user_to_block(at);

    void* prev_free = nullptr;
    void* curr_free = meta->first_free_block;

    while (curr_free != nullptr && curr_free < returning_block)
    {
        prev_free = curr_free;
        curr_free = block_of(curr_free)->next;
    }

    if (curr_free == returning_block)
    {
        return;
    }

    returning_block->next = curr_free;

    if (prev_free != nullptr)
    {
        block_of(prev_free)->next = returning_block;
    }
    else
    {
        meta->first_free_block = returning_block;
    }

    if (returning_block->next != nullptr)
    {
        auto* next_header = block_of(returning_block->next);
        char* end_of_current = reinterpret_cast<char*>(returning_block) + returning_block->size;

        if (end_of_current == reinterpret_cast<char*>(next_header))
        {
            returning_block->size += next_header->size;
            returning_block->next = next_header->next;
        }
    }

    if (prev_free != nullptr)
    {
        auto* prev_header = block_of(prev_free);
        char* end_of_prev = reinterpret_cast<char*>(prev_free) + prev_header->size;

        if (end_of_prev == reinterpret_cast<char*>(returning_block))
        {
            prev_header->size += returning_block->size;
            prev_header->next = returning_block->next;
        }
    }
}

inline void allocator_sorted_list::set_fit_mode(
    allocator_with_fit_mode::fit_mode mode)
{
    if (_trusted_memory == nullptr)
    {
        return;
    }

    auto* meta = meta_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);
    meta->mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info() const noexcept
{
    return get_blocks_info_inner();
}


std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> result;

    if (_trusted_memory == nullptr)
    {
        return result;
    }

    auto* meta = meta_of(_trusted_memory);

    char* current_block_char = static_cast<char*>(_trusted_memory) + allocator_metadata_size;
    char* end_of_memory = static_cast<char*>(_trusted_memory) + meta->total_size;

    while (current_block_char < end_of_memory)
    {
        auto* header = block_of(current_block_char);

        allocator_test_utils::block_info info;
        info.block_size = header->size - block_metadata_size;

        bool is_free = false;
        void* it = meta->first_free_block;
        while (it != nullptr)
        {
            if (it == current_block_char)
            {
                is_free = true;
                break;
            }
            it = block_of(it)->next;
        }

        info.is_block_occupied = !is_free;
        result.push_back(info);

        current_block_char += header->size;
    }

    return result;
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_begin() const noexcept
{
    return sorted_free_iterator(_trusted_memory);
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_end() const noexcept
{
    return sorted_free_iterator();
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::begin() const noexcept
{
    return sorted_iterator(_trusted_memory);
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::end() const noexcept
{
    return sorted_iterator();
}

bool allocator_sorted_list::sorted_free_iterator::operator==(const sorted_free_iterator &other) const noexcept
{
    return _free_ptr == other._free_ptr;
}

bool allocator_sorted_list::sorted_free_iterator::operator!=(const sorted_free_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_sorted_list::sorted_free_iterator &allocator_sorted_list::sorted_free_iterator::operator++() & noexcept
{
    if (_free_ptr != nullptr)
    {
        _free_ptr = block_of(_free_ptr)->next;
    }
    return *this;
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::sorted_free_iterator::operator++(int n)
{
    sorted_free_iterator temp = *this;
    ++(*this);
    return temp;
}

size_t allocator_sorted_list::sorted_free_iterator::size() const noexcept
{
    if (_free_ptr == nullptr) return 0;
    return block_of(_free_ptr)->size - block_metadata_size;
}

void *allocator_sorted_list::sorted_free_iterator::operator*() const noexcept
{
    return _free_ptr;
}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator() : _free_ptr(nullptr) {}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator(void *trusted)
{
    if (trusted == nullptr)
    {
        _free_ptr = nullptr;
    }
    else
    {
        _free_ptr = meta_of(trusted)->first_free_block;
    }
}

bool allocator_sorted_list::sorted_iterator::operator==(const sorted_iterator &other) const noexcept
{
    return _current_ptr == other._current_ptr;
}

bool allocator_sorted_list::sorted_iterator::operator!=(const sorted_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_sorted_list::sorted_iterator &allocator_sorted_list::sorted_iterator::operator++() & noexcept
{
    if (_current_ptr == nullptr || _trusted_memory == nullptr)
    {
        return *this;
    }

    auto* meta = meta_of(_trusted_memory);
    size_t sz = block_of(_current_ptr)->size;
    _current_ptr = static_cast<char*>(_current_ptr) + sz;

    if (_current_ptr >= static_cast<char*>(_trusted_memory) + meta->total_size)
    {
        _current_ptr = nullptr;
    }

    return *this;
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::sorted_iterator::operator++(int n)
{
    sorted_iterator temp = *this;
    ++(*this);
    return temp;
}

size_t allocator_sorted_list::sorted_iterator::size() const noexcept
{
    if (_current_ptr == nullptr) return 0;
    return block_of(_current_ptr)->size - block_metadata_size;
}

void *allocator_sorted_list::sorted_iterator::operator*() const noexcept
{
    if (_current_ptr == nullptr) return nullptr;
    return block_to_user(block_of(_current_ptr));
}

allocator_sorted_list::sorted_iterator::sorted_iterator() : _free_ptr(nullptr), _current_ptr(nullptr), _trusted_memory(nullptr) {}

allocator_sorted_list::sorted_iterator::sorted_iterator(void *trusted) : _trusted_memory(trusted), _free_ptr(nullptr)
{
    if (trusted != nullptr)
    {
        _current_ptr = static_cast<char*>(trusted) + allocator_metadata_size;
    }
    else
    {
        _current_ptr = nullptr;
    }
}

bool allocator_sorted_list::sorted_iterator::occupied() const noexcept
{
    if (_trusted_memory == nullptr || _current_ptr == nullptr)
    {
        return false;
    }

    void* it_free = meta_of(_trusted_memory)->first_free_block;
    while (it_free != nullptr)
    {
        if (it_free == _current_ptr)
        {
            return false;
        }
        it_free = block_of(it_free)->next;
    }
    return true;
}