int main(int argc, char *argv[]);


/** address: 0x10000418 */
int main(int argc, char *argv[])
{
    unsigned char local0; 		// m[g1 - 19]
    unsigned char local1; 		// m[g1 - 20]

    local0 = 1;
    if (argc <= 1) {
        local0 = 0;
    }
    local1 = (char) (local0);
    printf("Result is %d\n", ROTL((local1), 0) & 0xff);
    return ROTL((local1), 0) & 0xff;
}

