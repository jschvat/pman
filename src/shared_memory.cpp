#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "pman/shared_memory.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <stdexcept>
#include <system_error>
#include <utility>

namespace pman {

namespace {

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#ifndef MFD_HUGETLB
#define MFD_HUGETLB 0x0004U
#endif

int memfd_create_wrapper(const char* name, unsigned int flags) {
#ifdef SYS_memfd_create
    return static_cast<int>(syscall(SYS_memfd_create, name, flags));
#else
    (void)name;
    (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}

}  // namespace

SharedMemory::SharedMemory(std::string name, std::size_t size, SharedMemoryOptions options)
    : name_(std::move(name)), size_(size) {

    int flags = 0;
    bool created = false;

    switch (options.mode) {
    case SharedMemoryMode::Create:
        flags = O_CREAT | O_EXCL | O_RDWR;
        created = true;
        break;
    case SharedMemoryMode::Open:
        flags = O_RDWR;
        break;
    case SharedMemoryMode::OpenOrCreate:
        flags = O_CREAT | O_RDWR;
        break;
    }

    fd_ = shm_open(name_.c_str(), flags, options.permissions);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "shm_open");
    }

    if (options.mode == SharedMemoryMode::OpenOrCreate) {
        struct stat st;
        if (fstat(fd_, &st) == 0 && st.st_size == 0) {
            created = true;
        }
    }

    if (created || options.mode == SharedMemoryMode::Create) {
        if (ftruncate(fd_, static_cast<off_t>(size_)) != 0) {
            int err = errno;
            ::close(fd_);
            shm_unlink(name_.c_str());
            throw std::system_error(err, std::generic_category(), "ftruncate");
        }
        owner_ = true;
    } else {
        struct stat st;
        if (fstat(fd_, &st) != 0) {
            int err = errno;
            ::close(fd_);
            throw std::system_error(err, std::generic_category(), "fstat");
        }
        size_ = static_cast<std::size_t>(st.st_size);
    }

    int mmap_flags = MAP_SHARED;
    if (options.hugePages) {
#ifdef MAP_HUGETLB
        mmap_flags |= MAP_HUGETLB;
#endif
    }

    ptr_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, mmap_flags, fd_, 0);
    if (ptr_ == MAP_FAILED) {
        int err = errno;
        ::close(fd_);
        if (owner_) {
            shm_unlink(name_.c_str());
        }
        throw std::system_error(err, std::generic_category(), "mmap");
    }

    if (options.lockInMemory) {
        if (mlock(ptr_, size_) != 0) {
            int err = errno;
            munmap(ptr_, size_);
            ::close(fd_);
            if (owner_) {
                shm_unlink(name_.c_str());
            }
            throw std::system_error(err, std::generic_category(), "mlock");
        }
    }
}

SharedMemory::~SharedMemory() {
    if (ptr_ && ptr_ != MAP_FAILED) {
        munmap(ptr_, size_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

SharedMemory::SharedMemory(SharedMemory&& other) noexcept
    : name_(std::move(other.name_)),
      ptr_(other.ptr_),
      size_(other.size_),
      fd_(other.fd_),
      owner_(other.owner_) {
    other.ptr_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
    other.owner_ = false;
}

SharedMemory& SharedMemory::operator=(SharedMemory&& other) noexcept {
    if (this != &other) {
        if (ptr_ && ptr_ != MAP_FAILED) {
            munmap(ptr_, size_);
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }

        name_ = std::move(other.name_);
        ptr_ = other.ptr_;
        size_ = other.size_;
        fd_ = other.fd_;
        owner_ = other.owner_;

        other.ptr_ = nullptr;
        other.size_ = 0;
        other.fd_ = -1;
        other.owner_ = false;
    }
    return *this;
}

void SharedMemory::flush() {
    if (ptr_ && ptr_ != MAP_FAILED) {
        msync(ptr_, size_, MS_SYNC);
    }
}

void SharedMemory::flush(std::size_t offset, std::size_t length) {
    if (ptr_ && ptr_ != MAP_FAILED && offset < size_) {
        std::size_t actualLength = std::min(length, size_ - offset);
        char* addr = static_cast<char*>(ptr_) + offset;

        long pageSize = sysconf(_SC_PAGESIZE);
        std::size_t alignedOffset = (offset / static_cast<std::size_t>(pageSize)) * static_cast<std::size_t>(pageSize);
        char* alignedAddr = static_cast<char*>(ptr_) + alignedOffset;
        std::size_t alignedLength = actualLength + (offset - alignedOffset);

        msync(alignedAddr, alignedLength, MS_SYNC);
    }
}

void SharedMemory::unlink(const std::string& name) {
    if (shm_unlink(name.c_str()) != 0 && errno != ENOENT) {
        throw std::system_error(errno, std::generic_category(), "shm_unlink");
    }
}

bool SharedMemory::exists(const std::string& name) {
    int fd = shm_open(name.c_str(), O_RDONLY, 0);
    if (fd >= 0) {
        ::close(fd);
        return true;
    }
    return false;
}

SharedMemory SharedMemory::createAnonymous(std::size_t size, bool hugePages) {
    unsigned int flags = MFD_CLOEXEC;
    if (hugePages) {
        flags |= MFD_HUGETLB;
    }

    int fd = memfd_create_wrapper("pman_anon_shm", flags);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "memfd_create");
    }

    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        int err = errno;
        ::close(fd);
        throw std::system_error(err, std::generic_category(), "ftruncate");
    }

    int mmap_flags = MAP_SHARED;
    if (hugePages) {
#ifdef MAP_HUGETLB
        mmap_flags |= MAP_HUGETLB;
#endif
    }

    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, mmap_flags, fd, 0);
    if (ptr == MAP_FAILED) {
        int err = errno;
        ::close(fd);
        throw std::system_error(err, std::generic_category(), "mmap");
    }

    SharedMemory shm;
    shm.name_ = "";
    shm.ptr_ = ptr;
    shm.size_ = size;
    shm.fd_ = fd;
    shm.owner_ = true;
    return shm;
}

}  // namespace pman
