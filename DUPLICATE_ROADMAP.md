

What "optimized for this project" actually means here:

A general hash function like xxHash handles any input. Your use case has constraints you can exploit:

You're always reading from disk sequentially — your hash can be designed around streaming chunks, not random access
You hash many files back-to-back — you can reuse a persistent state object instead of reinitializing per file, avoiding setup cost
After Checkpoint 5 (partial hashing), you hash only the first 4 KB of most files — a function tuned for short inputs can outperform one tuned for large ones
What you'd actually be building:

Something like a streaming 64-bit hash with explicit SIMD — process 32 bytes per loop iteration using the same idea as xxHash but written by you, for this workload. You'd learn:

How bit mixing works (why you XOR, rotate, and multiply specific constants)
What SIMD intrinsics look like in C++ (__m256i, _mm256_xor_si256)
Why the magic constants in xxHash are those specific numbers (they're chosen to maximize avalanche effect — a 1-bit input change flips ~50% of output bits)




TODO
1. generator instead of vector 
2. Mmap?
3. read chapter 9
4. read chapter 12
5. multi-sample hashing
6. avoid reopening tiny files
coroutines with iouring? but what of windows 



