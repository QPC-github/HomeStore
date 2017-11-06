/*
 * MemAllocator.hpp
 *
 *  Created on: 16-May-2016
 *      Author: hkadayam
 */

//  Copyright © 2016 Kadayam, Hari. All rights reserved.
#pragma once

#include "omds/memory/freelist_allocator.hpp"
#include "omds/utility/useful_defs.hpp"

namespace omds {

#define FREELIST_CACHE_COUNT         10000

template <typename T, size_t CacheCount = FREELIST_CACHE_COUNT>
class ObjectAllocator
{
public:
    ObjectAllocator() {
        m_allocator = std::make_unique< omds::FreeListAllocator< FREELIST_CACHE_COUNT, sizeof(T) > >();
    }

    template <class... Args>
    static T *make_object(Args &&... args) {
        uint8_t *mem = get_obj_allocator()->m_allocator->allocate(sizeof(T));
        T *ptr = new (mem) T(std::forward<Args>(args)...);
        return ptr;
    }

    static void deallocate(T *mem) {
        mem->~T();
        get_obj_allocator()->m_allocator->deallocate((uint8_t *)mem, sizeof(T));
    }

    static std::unique_ptr< ObjectAllocator< T, CacheCount > > obj_allocator;
private:
    omds::FreeListAllocator< FREELIST_CACHE_COUNT, sizeof(T) > *get_freelist_allocator() {
        return m_allocator.get();
    }

private:
    std::unique_ptr< omds::FreeListAllocator< FREELIST_CACHE_COUNT, sizeof(T) > > m_allocator;

    static ObjectAllocator< T, CacheCount > *get_obj_allocator() {
        if (unlikely((obj_allocator == nullptr))) {
            obj_allocator = std::make_unique< ObjectAllocator< T, CacheCount > >();
        }
        return obj_allocator.get();
    }
};

template <typename T, size_t CacheCount >
std::unique_ptr< ObjectAllocator< T, CacheCount > > ObjectAllocator<T, CacheCount>::obj_allocator = nullptr;
} // namespace omds