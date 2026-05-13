#include <not_implemented.h>
#include "../include/allocator_global_heap.h"

allocator_global_heap::allocator_global_heap()
{

}

[[nodiscard]] void *allocator_global_heap::do_allocate_sm(size_t size)
{
    std::lock_guard<std::mutex> lock(mtx);

    if (size == 0)
    {
        return nullptr;
    }
    
    void *ptr = ::operator new(size, std::nothrow);
    
    if (ptr == nullptr)
    {
        throw std::bad_alloc();
    }
    
    return ptr;
}

void allocator_global_heap::do_deallocate_sm(void *at)
{
    std::lock_guard<std::mutex> lock(mtx);
    
    if (at == nullptr)
    {
        return;
    }
    
    ::operator delete(at);
}

allocator_global_heap::~allocator_global_heap()
{

}

allocator_global_heap::allocator_global_heap(const allocator_global_heap &other)
    : smart_mem_resource(other)
{

}

allocator_global_heap &allocator_global_heap::operator=(const allocator_global_heap &other)
{
    if (this != &other)
    {
        smart_mem_resource::operator=(other);
    }
    return *this;
}

bool allocator_global_heap::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return this == &other;
}

allocator_global_heap::allocator_global_heap(allocator_global_heap &&other) noexcept
    : smart_mem_resource(std::move(other))
{

}

allocator_global_heap &allocator_global_heap::operator=(allocator_global_heap &&other) noexcept
{
    if (this != &other)
    {
        smart_mem_resource::operator=(std::move(other));
    }
    return *this;
}