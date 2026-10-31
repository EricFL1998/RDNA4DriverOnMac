#ifndef RDNA4CommandBuffer_h
#define RDNA4CommandBuffer_h

// The implementation classes are declared together in RDNA4Device.h; this
// header exists so a translation unit can import just the type it implements.
#import "RDNA4Device.h"

@interface RDNA4CommandBuffer (Rendering)
- (void)recordEncodingError:(NSError*)error;
@end

#endif /* RDNA4CommandBuffer_h */
