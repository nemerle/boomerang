int main(union { int; char *[] *; } argc, union { int; char *[] *; } argv);


/** address: 0x10000418 */
int main(union { int; char *[] *; } argc, union { int; char *[] *; } argv)
{
    int CR0; 		// r64
    int CR1; 		// r65
    int CR2; 		// r66
    int CR3; 		// r67
    int CR4; 		// r68
    int CR5; 		// r69
    int CR6; 		// r70
    __size32 CR7; 		// r71
    unsigned int g0; 		// r0
    unsigned int g0_1; 		// r0{45}
    int g3; 		// r3
    char * *g3_1; 		// r3
    int g5; 		// r5
    int local0; 		// m[g1 - 28]

    if (argc <= 1) {
        if (argc >= 0) {
            if (argc <= 0) {
                g0_1 = *(unsigned char*)g5;
                if ((ROTL(g0_1, 0) & 0xff) == 47) {
bb0x100006e8:
                    local0 = 1;
                }
                else {
                    g3_1 = main(-61, ROTL(g0, 0) & 0xff);
                    g3 = main(0, g3_1);
                    if (g3 != 0) {
                        goto bb0x100006e8;
                    }
                }
            }
            else {
                g3 = main(2, 2);
                local0 = g3;
            }
        }
        else {
            if (argc >= -72) {
                if (argc >= -50) {
                    g3 = main((ROTL(((CR0 << 28) + (CR1 << 24) + (CR2 << 20) + (CR3 << 16) + (CR4 << 12) + (CR5 << 8) + (CR6 << 4) + CR7), 31) & 0x1) + argc, argv);
                    local0 = g3;
                }
                else {
                    g0 = *(unsigned char*)g5;
                    if (argv != (ROTL(g0, 0) & 0xff)) {
                        g3 = main(-65, argv);
                        local0 = g3;
                    }
                    else {
                        g0 = *(unsigned char*)(g5 + 31);
                        g3 = putchar(ROTL(g0, 0) & 0xff);
                        local0 = g3;
                    }
                }
            }
            else {
                g3 = main(argv, argc);
                local0 = g3;
            }
        }
    }
    else {
        if (argc <= 2) {
            main(-86, 0);
            main(-87, 1 - argv);
            main(-79, -13);
        }
        if (argc < argv) {
            main(argc + 1, argv);
        }
        g3 = main(-94, argc - 27);
        if (g3 == 0 || argc != 2) {
            local0 = 16;
        }
        else {
            if (argv > 12) {
                local0 = 9;
            }
            else {
                g3 = main(2, argv + 1);
                local0 = g3;
            }
        }
    }
    return local0;
}

