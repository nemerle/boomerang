int main(int argc, char *argv[]);


/** address: 0x100004f8 */
int main(int argc, char *argv[])
{
    int CR0; 		// r64
    int CR1; 		// r65
    int CR2; 		// r66
    int CR3; 		// r67
    int CR4; 		// r68
    int CR5; 		// r69
    int CR6; 		// r70
    __size32 CR7; 		// r71
    __size32 local0; 		// m[g1 - 40]
    __size32 local1; 		// m[g1 - 36]
    __size32 local2; 		// m[g1 - 32]
    __size32 local3; 		// m[g1 - 28]
    __size32 local4; 		// m[g1 - 24]
    __size32 local5; 		// m[g1 - 48]

    local0 = 1;
    if (argc <= 1) {
        local0 = 0;
    }
    if (local0 == 0) {
        local1 = 0x10000434;
    }
    else {
        local1 = 0x10000418;
    }
    if (local0 == 0) {
        local2 = 0x1000046c;
    }
    else {
        local2 = 0x10000450;
    }
    if (local0 == 0) {
        local3 = 0x100004a4;
    }
    else {
        local3 = 0x10000488;
    }
    if (local0 == 0) {
        local4 = 0x100004dc;
    }
    else {
        local4 = 0x100004c0;
    }
    if (argc <= 1) {
        local5 = 0;
        if (local1 == 0x10000434 && local2 == 0x1000046c && local3 == 0x100004a4 && local4 == 0x100004dc) {
            local5 = 1;
        }
    }
    else {
        local5 = 0;
        if (local1 == 0x10000418 && local2 == 0x10000450 && local3 == 0x10000488 && local4 == 0x100004c0) {
            local5 = 1;
        }
    }
    if (local5 == 0) {
        printf("Failed!\n");
    }
    else {
        printf("Pass\n");
    }
    return ROTL(((CR0 << 28) + (CR1 << 24) + (CR2 << 20) + (CR3 << 16) + (CR4 << 12) + (CR5 << 8) + (CR6 << 4) + CR7), 31) & 0x1;
}

