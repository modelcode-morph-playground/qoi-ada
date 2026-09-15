# qoi-ada

Ada implementation of the "Quite OK Image" (QOI) codec, based on the
[QOI](https://qoiformat.org/) format specification V1.

Pure-Ada source mirror of [Fabien-Chouteau/qoi-spark](https://github.com/Fabien-Chouteau/qoi-spark)
(MIT), used as the source repository for the modelcode end-to-end regression's
**Ada → C++** migration scenario (`qoi_ada_to_cpp`).

To call the `Encode`/`Decode` procedure you have to provide a large enough
output buffer. If the provided output buffer is not large enough, each
procedure will return with an `Output_Size` of zero. For `Encode` the minimum
size for the output buffer is given by the `Encode_Worst_Case` function based
on the dimensions of the image and the number of channels. For `Decode` you
should use the `Get_Desc` procedure to get the image specification and then the
exact output size will be `Desc.Width * Desc.Height * Desc.Channels`.
