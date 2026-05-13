#ifndef MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_SORTED_LIST_H
#define MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_SORTED_LIST_H

#include <pp_allocator.h>
#include <allocator_test_utils.h>
#include <allocator_with_fit_mode.h>
#include <iterator>
#include <mutex>

class allocator_sorted_list final:
    public smart_mem_resource,
    public allocator_test_utils,
    public allocator_with_fit_mode
{

private:
    
    void *_trusted_memory;

    struct allocator_meta
    {
        std::pmr::memory_resource* parent_allocator;
        fit_mode mode;
        size_t total_size;
        mutable std::mutex mtx;
        void* first_free_block;
    };

    struct block_header
    {
        size_t size;
        void* next;
    };

    static constexpr size_t allocator_metadata_size = (sizeof(allocator_meta) + alignof(std::max_align_t) - 1) & ~(alignof(std::max_align_t) - 1);
    static constexpr size_t block_metadata_size = (sizeof(block_header) + alignof(std::max_align_t) - 1) & ~(alignof(std::max_align_t) - 1);

    static allocator_meta* meta_of(void* ptr)
    {
        return reinterpret_cast<allocator_meta*>(ptr);
    }

    static block_header* block_of(void* ptr)
    {
        return reinterpret_cast<block_header*>(ptr);
    }

    static void* block_to_user(block_header* b)
    {
        return reinterpret_cast<char*>(b) + block_metadata_size;
    }

    static block_header* user_to_block(void* user)
    {
        return reinterpret_cast<block_header*>(reinterpret_cast<char*>(user) - block_metadata_size);
    }

public:

    explicit allocator_sorted_list(
            size_t space_size,
            std::pmr::memory_resource *parent_allocator = nullptr,
            allocator_with_fit_mode::fit_mode allocate_fit_mode = allocator_with_fit_mode::fit_mode::first_fit);
    
    allocator_sorted_list(
        allocator_sorted_list const &other);
    
    allocator_sorted_list &operator=(
        allocator_sorted_list const &other);

    allocator_sorted_list(
        allocator_sorted_list &&other) noexcept;
    
    allocator_sorted_list &operator=(
        allocator_sorted_list &&other) noexcept;

    ~allocator_sorted_list() override;

private:
    
    [[nodiscard]] void *do_allocate_sm(
        size_t size) override;
    
    void do_deallocate_sm(
        void *at) override;

    bool do_is_equal(const std::pmr::memory_resource&) const noexcept override;
    
    inline void set_fit_mode(
        allocator_with_fit_mode::fit_mode mode) override;

    std::vector<allocator_test_utils::block_info> get_blocks_info() const noexcept override;

private:

    std::vector<allocator_test_utils::block_info> get_blocks_info_inner() const override;

    struct search_res
    {
        void* target;
        void* prev;
    };

    search_res first_fit(size_t size) const;
    search_res best_fit(size_t size) const;
    search_res worst_fit(size_t size) const;

    class sorted_free_iterator
    {
        void* _free_ptr;

    public:

        using iterator_category = std::forward_iterator_tag;
        using value_type = void*;
        using reference = void*&;
        using pointer = void**;
        using difference_type = ptrdiff_t;

        bool operator==(const sorted_free_iterator&) const noexcept;

        bool operator!=(const sorted_free_iterator&) const noexcept;

        sorted_free_iterator& operator++() & noexcept;

        sorted_free_iterator operator++(int n);

        size_t size() const noexcept;

        void* operator*() const noexcept;

        sorted_free_iterator();

        sorted_free_iterator(void* trusted);
    };

    class sorted_iterator
    {
        void* _free_ptr;
        void* _current_ptr;
        void* _trusted_memory;

    public:

        using iterator_category = std::forward_iterator_tag;
        using value_type = void*;
        using reference = void*&;
        using pointer = void**;
        using difference_type = ptrdiff_t;

        bool operator==(const sorted_iterator&) const noexcept;

        bool operator!=(const sorted_iterator&) const noexcept;

        sorted_iterator& operator++() & noexcept;

        sorted_iterator operator++(int n);

        size_t size() const noexcept;

        void* operator*() const noexcept;

        bool occupied()const noexcept;

        sorted_iterator();

        sorted_iterator(void* trusted);
    };

    friend class sorted_iterator;
    friend class sorted_free_iterator;

    sorted_free_iterator free_begin() const noexcept;
    sorted_free_iterator free_end() const noexcept;

    sorted_iterator begin() const noexcept;
    sorted_iterator end() const noexcept;
};

#endif //MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_SORTED_LIST_H