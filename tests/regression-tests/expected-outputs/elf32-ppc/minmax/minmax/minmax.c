int main(int argc, char *argv[]);


/** address: 0x1000040c */
int main(int argc, char *argv[])
{
    int g3; 		// r3
    int local0; 		// argc{4}

    local0 = argc;
    if (argc >= 3) {
        if (argc <= 3) {
lab_4:
            argc = local0;
            printf("MinMax adjusted number of arguments is %d\n", argc);
        }
        else {
            printf("MinMax adjusted number of arguments is %d\n", 3);
        }
    }
    else {
        g3 = -2;
        local0 = g3;
        goto lab_4;
    }
    return 0;
}

