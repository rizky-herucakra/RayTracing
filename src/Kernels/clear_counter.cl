__kernel void ClearCounter(__global uint* counter)
{
    uint global_id = get_global_id(0);

    if (global_id == 0)
    {
        counter[0] = 0;
    }
}
