# image-compression
A from-scratch, dependency-free JPEG-style image compression tool written in C++17. It implements the full classical compression pipeline ; color space conversion, chroma subsampling, DCT, quantization, zigzag scanning, and RLE, producing its own compact binary format (.jc) and reconstructing images back to PPM.
