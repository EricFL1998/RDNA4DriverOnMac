__kernel void vadd2(__global const float *a, __global const float *b, __global float *r)
{
    unsigned i = __builtin_amdgcn_workgroup_id_x() * 256u + __builtin_amdgcn_workitem_id_x();
    r[i] = a[i] + b[i];
}
