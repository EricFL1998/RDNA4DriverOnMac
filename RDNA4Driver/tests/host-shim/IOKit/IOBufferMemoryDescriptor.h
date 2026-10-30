#pragma once

class IOBufferMemoryDescriptor {
public:
    virtual ~IOBufferMemoryDescriptor() = default;
    virtual void complete() {}
    virtual void release() {}
};
