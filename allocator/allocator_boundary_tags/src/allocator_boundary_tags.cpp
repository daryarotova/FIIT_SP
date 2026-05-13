#include <not_implemented.h>
#include "../include/allocator_boundary_tags.h"

allocator_boundary_tags::~allocator_boundary_tags()
{
    if (_trusted_memory != nullptr)
    {
        allocator_meta *meta = get_meta(_trusted_memory);
        std::pmr::memory_resource *parent = meta->parent_allocator;
        size_t total_size = meta->total_size;

        meta->mtx.~mutex();

        parent->deallocate(_trusted_memory, total_size);
        _trusted_memory = nullptr;
    }
}

allocator_boundary_tags::allocator_boundary_tags(
    allocator_boundary_tags &&other) noexcept
{
    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;

    if (_trusted_memory != nullptr)
    {
        allocator_meta *meta = get_meta(_trusted_memory);
        std::lock_guard<std::mutex> lock(meta->mtx);

        block_meta *current = reinterpret_cast<block_meta *>(meta->first_block);
        while (current != nullptr)
        {
            if (current->allocator_ptr != nullptr)
            {
                current->allocator_ptr = this;
            }
            current = reinterpret_cast<block_meta *>(current->next);
        }
    }
}

allocator_boundary_tags &allocator_boundary_tags::operator=(
    allocator_boundary_tags &&other) noexcept
{
    if (this != &other)
    {
        this->~allocator_boundary_tags();

        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;

        if (_trusted_memory != nullptr)
        {
            allocator_meta *meta = get_meta(_trusted_memory);
            std::lock_guard<std::mutex> lock(meta->mtx);

            block_meta *current = reinterpret_cast<block_meta *>(meta->first_block);
            while (current != nullptr)
            {
                if (current->allocator_ptr != nullptr)
                {
                    current->allocator_ptr = this;
                }
                current = reinterpret_cast<block_meta *>(current->next);
            }
        }
    }
    return *this;
}


/** If parent_allocator* == nullptr you should use std::pmr::get_default_resource()
 */
allocator_boundary_tags::allocator_boundary_tags(
    size_t space_size,
    std::pmr::memory_resource *parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (parent_allocator == nullptr)
    {
        parent_allocator = std::pmr::get_default_resource();
    }

    size_t total_size = space_size + allocator_metadata_size;
    _trusted_memory = parent_allocator->allocate(total_size);

    if (_trusted_memory == nullptr)
    {
        throw std::bad_alloc();
    }

    allocator_meta *meta = new (_trusted_memory) allocator_meta();
    meta->parent_allocator = parent_allocator;
    meta->mode = allocate_fit_mode;
    meta->total_size = total_size;

    void *first_block_ptr = reinterpret_cast<char *>(_trusted_memory) + allocator_metadata_size;
    meta->first_block = first_block_ptr;

    block_meta *first_block = new (first_block_ptr) block_meta();
    first_block->size = space_size;
    first_block->allocator_ptr = nullptr;
    first_block->prev = nullptr;
    first_block->next = nullptr;
}

[[nodiscard]] void *allocator_boundary_tags::do_allocate_sm(
    size_t size)
{
    allocator_meta *meta = get_meta(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);

    block_meta *target_block = nullptr;
    block_meta *current = reinterpret_cast<block_meta *>(meta->first_block);

    if (meta->mode == allocator_with_fit_mode::fit_mode::first_fit)
    {
        while (current != nullptr)
        {
            if (current->allocator_ptr == nullptr && current->size >= occupied_block_metadata_size + size)
            {
                target_block = current;
                break;
            }
            current = reinterpret_cast<block_meta *>(current->next);
        }
    }
    else if (meta->mode == allocator_with_fit_mode::fit_mode::the_best_fit)
    {
        size_t min_diff = size_t(-1);
        while (current != nullptr)
        {
            if (current->allocator_ptr == nullptr && current->size >= occupied_block_metadata_size + size)
            {
                size_t diff = current->size - (occupied_block_metadata_size + size);
                if (diff < min_diff)
                {
                    min_diff = diff;
                    target_block = current;
                }
            }
            current = reinterpret_cast<block_meta *>(current->next);
        }
    }
    else if (meta->mode == allocator_with_fit_mode::fit_mode::the_worst_fit)
    {
        size_t max_diff = 0;
        bool found = false;
        while (current != nullptr)
        {
            if (current->allocator_ptr == nullptr && current->size >= occupied_block_metadata_size + size)
            {
                size_t diff = current->size - (occupied_block_metadata_size + size);
                if (!found || diff > max_diff)
                {
                    max_diff = diff;
                    target_block = current;
                    found = true;
                }
            }
            current = reinterpret_cast<block_meta *>(current->next);
        }
    }

    if (target_block == nullptr)
    {
        throw std::bad_alloc();
    }

    size_t needed_size = size + occupied_block_metadata_size;
    if (target_block->size >= needed_size + occupied_block_metadata_size)
    {
        size_t old_size = target_block->size;
        target_block->size = needed_size;

        void *new_free_ptr = reinterpret_cast<char *>(target_block) + needed_size;
        block_meta *new_free_block = new (new_free_ptr) block_meta();
        new_free_block->size = old_size - needed_size;
        new_free_block->allocator_ptr = nullptr;
        new_free_block->prev = target_block;
        new_free_block->next = target_block->next;

        if (target_block->next != nullptr)
        {
            get_block(target_block->next)->prev = new_free_block;
        }
        target_block->next = new_free_block;
    }

    target_block->allocator_ptr = this;
    return get_user_ptr(target_block);
}

void allocator_boundary_tags::do_deallocate_sm(
    void *at)
{
    if (at == nullptr) return;

    allocator_meta *meta = get_meta(_trusted_memory);

    char* pool_start = static_cast<char*>(_trusted_memory) + allocator_metadata_size;
    char* pool_end = static_cast<char*>(_trusted_memory) + meta->total_size;
    char* user_ptr = static_cast<char*>(at);

    if (user_ptr < pool_start || user_ptr >= pool_end)
    {
        throw std::logic_error("allocator_boundary_tags: pointer is out of this allocator range");
    }

    std::lock_guard<std::mutex> lock(meta->mtx);

    block_meta *target = get_block_from_user(at);

    if (target->allocator_ptr != this)
    {
        throw std::logic_error("allocator_boundary_tags: allocator_boundary_tags: pointer does not belong to this instance");
    }

    target->allocator_ptr = nullptr;

    if (target->next != nullptr)
    {
        block_meta *next_block = get_block(target->next);
        if (next_block->allocator_ptr == nullptr)
        {
            target->size += next_block->size;
            target->next = next_block->next;
            if (next_block->next != nullptr)
            {
                get_block(next_block->next)->prev = target;
            }
        }
    }

    if (target->prev != nullptr)
    {
        block_meta *prev_block = get_block(target->prev);
        if (prev_block->allocator_ptr == nullptr)
        {
            prev_block->size += target->size;
            prev_block->next = target->next;
            if (target->next != nullptr)
            {
                get_block(target->next)->prev = prev_block;
            }
        }
    }
}

inline void allocator_boundary_tags::set_fit_mode(
    allocator_with_fit_mode::fit_mode mode)
{
    allocator_meta *meta = get_meta(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);
    meta->mode = mode;
}


std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info() const
{
    return get_blocks_info_inner();
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::begin() const noexcept
{
    return boundary_iterator(_trusted_memory);
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::end() const noexcept
{
    boundary_iterator it;
    it._trusted_memory = _trusted_memory;
    it._occupied_ptr = nullptr;
    it._occupied = false;
    return it;
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> result;
    allocator_meta *meta = get_meta(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);

    block_meta *current = reinterpret_cast<block_meta *>(meta->first_block);
    while (current != nullptr)
    {
        allocator_test_utils::block_info info;
        info.block_size = current->size;
        info.is_block_occupied = (current->allocator_ptr != nullptr);
        result.push_back(info);
        current = reinterpret_cast<block_meta *>(current->next);
    }
    return result;
}

allocator_boundary_tags::allocator_boundary_tags(const allocator_boundary_tags &other)
{
    if (other._trusted_memory == nullptr)
    {
        _trusted_memory = nullptr;
        return;
    }

    allocator_meta *other_meta = get_meta(other._trusted_memory);
    std::lock_guard<std::mutex> lock(other_meta->mtx);

    size_t total_size = other_meta->total_size;
    _trusted_memory = other_meta->parent_allocator->allocate(total_size);

    if (_trusted_memory == nullptr)
    {
        throw std::bad_alloc();
    }

    std::memcpy(_trusted_memory, other._trusted_memory, total_size);

    allocator_meta *meta = get_meta(_trusted_memory);
    new (&meta->mtx) std::mutex();
    ptrdiff_t offset = reinterpret_cast<char *>(_trusted_memory) - reinterpret_cast<char *>(other._trusted_memory);
    
    meta->first_block = reinterpret_cast<char *>(meta->first_block) + offset;
    
    block_meta *current = reinterpret_cast<block_meta *>(meta->first_block);
    while (current != nullptr)
    {
        if (current->allocator_ptr != nullptr)
        {
            current->allocator_ptr = this;
        }
        
        if (current->prev != nullptr)
        {
            current->prev = reinterpret_cast<char *>(current->prev) + offset;
        }
        
        if (current->next != nullptr)
        {
            current->next = reinterpret_cast<char *>(current->next) + offset;
        }
        
        current = reinterpret_cast<block_meta *>(current->next);
    }
}

allocator_boundary_tags &allocator_boundary_tags::operator=(const allocator_boundary_tags &other)
{
    if (this != &other)
    {
        allocator_boundary_tags temp(other);
        *this = std::move(temp);
    }
    return *this;
}

bool allocator_boundary_tags::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return this == &other;
}

bool allocator_boundary_tags::boundary_iterator::operator==(
    const allocator_boundary_tags::boundary_iterator &other) const noexcept
{
    return _occupied_ptr == other._occupied_ptr && _trusted_memory == other._trusted_memory;
}

bool allocator_boundary_tags::boundary_iterator::operator!=(
    const allocator_boundary_tags::boundary_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_boundary_tags::boundary_iterator &allocator_boundary_tags::boundary_iterator::operator++() & noexcept
{
    if (_occupied_ptr != nullptr)
    {
        block_meta *block = reinterpret_cast<block_meta *>(_occupied_ptr);
        _occupied_ptr = block->next;
        if (_occupied_ptr != nullptr)
        {
            _occupied = (reinterpret_cast<block_meta *>(_occupied_ptr)->allocator_ptr != nullptr);
        }
        else
        {
            _occupied = false;
        }
    }
    return *this;
}

allocator_boundary_tags::boundary_iterator &allocator_boundary_tags::boundary_iterator::operator--() & noexcept
{
    if (_occupied_ptr != nullptr)
    {
        block_meta *block = reinterpret_cast<block_meta *>(_occupied_ptr);
        if (block->prev != nullptr)
        {
            _occupied_ptr = block->prev;
            _occupied = (reinterpret_cast<block_meta *>(_occupied_ptr)->allocator_ptr != nullptr);
        }
    }
    else if (_trusted_memory != nullptr)
    {
        allocator_meta *meta = reinterpret_cast<allocator_meta *>(_trusted_memory);
        block_meta *current = reinterpret_cast<block_meta *>(meta->first_block);
        while (current != nullptr && current->next != nullptr)
        {
            current = reinterpret_cast<block_meta *>(current->next);
        }
        _occupied_ptr = current;
        if (_occupied_ptr != nullptr)
        {
            _occupied = (reinterpret_cast<block_meta *>(_occupied_ptr)->allocator_ptr != nullptr);
        }
    }
    return *this;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator++(int n)
{
    boundary_iterator temp = *this;
    ++(*this);
    return temp;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator--(int n)
{
    boundary_iterator temp = *this;
    --(*this);
    return temp;
}

size_t allocator_boundary_tags::boundary_iterator::size() const noexcept
{
    if (_occupied_ptr != nullptr)
    {
        return reinterpret_cast<block_meta *>(_occupied_ptr)->size;
    }
    return 0;
}

bool allocator_boundary_tags::boundary_iterator::occupied() const noexcept
{
    return _occupied;
}

void *allocator_boundary_tags::boundary_iterator::operator*() const noexcept
{
    if (_occupied_ptr != nullptr)
    {
        return reinterpret_cast<char *>(_occupied_ptr) + occupied_block_metadata_size;
    }
    return nullptr;
}

allocator_boundary_tags::boundary_iterator::boundary_iterator()
    : _occupied_ptr(nullptr), _occupied(false), _trusted_memory(nullptr)
{
    
}

allocator_boundary_tags::boundary_iterator::boundary_iterator(void *trusted)
    : _trusted_memory(trusted)
{
    if (_trusted_memory != nullptr)
    {
        allocator_meta *meta = reinterpret_cast<allocator_meta *>(_trusted_memory);
        _occupied_ptr = meta->first_block;
        if (_occupied_ptr != nullptr)
        {
            _occupied = (reinterpret_cast<block_meta *>(_occupied_ptr)->allocator_ptr != nullptr);
        }
        else
        {
            _occupied = false;
        }
    }
    else
    {
        _occupied_ptr = nullptr;
        _occupied = false;
    }
}

void *allocator_boundary_tags::boundary_iterator::get_ptr() const noexcept
{
    return _occupied_ptr;
}
