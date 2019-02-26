int main(int argc, char *argv[]);


/** address: 0x10000418 */
int main(int argc, char *argv[])
{
    int g0; 		// r0
    int local0; 		// m[g1 - 24]
    int local1; 		// m[g1 - 20]

    local0 = 0;
    local1 = 0;
    while (local1 <= 9) {
        g0 = *((ROTL(local1, 2) & ~0x3) + 0x10010958);
        local0 += g0;
        local1++;
    }
    printf("Sum is %d\n", local0);
    return 0;
}

