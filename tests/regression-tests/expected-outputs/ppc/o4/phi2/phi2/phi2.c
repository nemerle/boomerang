int main(int argc, char *argv[]);


/** address: 0x100004f0 */
int main(int argc, char *argv[])
{
    int g3; 		// r3
    int g30; 		// r30
    int g31; 		// r31
    int g3_1; 		// r3
    int g5; 		// r5

    g5 = *(argv + 4);
    if (argc > 2) {
        g3 = strlen(g5);
        g31 = g3;
        g30 = g3;
        printf("%d", ROTL(g3, 1) & ~0x1);
    }
    else {
        g3_1 = strlen(g5);
        g31 = g3_1;
    }
    printf("%d, %d", g31, g30);
    printf("%d\n", g31);
    return 0;
}

