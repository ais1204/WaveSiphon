#pragma once

// Minimal owning COM pointer (avoids depending on WRL/ATL so the project
// builds the same way with MSVC and MinGW).
template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T* Get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    // Releases the current pointer and returns the slot for an out-parameter.
    T** Put() {
        Reset();
        return &ptr_;
    }
    void** PutVoid() { return reinterpret_cast<void**>(Put()); }

    void Reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_ = nullptr;
};
