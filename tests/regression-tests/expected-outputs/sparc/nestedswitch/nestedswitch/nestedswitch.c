int main(unsigned int argc, char *argv[]);


/** address: 0x000106a4 */
int main(unsigned int argc, char *argv[])
{
    int o0; 		// r8

    if (argc > 7) {
bb0x106d0:
        o0 = "Other!";
        break;
    }
    switch(argc) {
    case 1:
    case 0:
        goto bb0x106d0;
    case 7:
bb0x106dc:
        o0 = "Seven!";
        break;
    case 2:
bb0x106e8:
        o0 = "Two!";
        break;
    case 3:
bb0x106f4:
        o0 = "Three!";
        break;
    case 5:
bb0x10700:
        o0 = "Five!";
        break;
    case 6:
bb0x1070c:
        o0 = "Six!";
        break;
    case 4:
        if (7 - argc <= 5) {
            switch(7 - argc) {
            case 0:
                goto bb0x106dc;
            case 5:
                goto bb0x106e8;
            case 4:
                goto bb0x106f4;
            case 2:
                goto bb0x10700;
            case 1:
                goto bb0x1070c;
            case 3:
                o0 = "Four!";
                break;
            }
            goto bb0x10744;
        }
        goto bb0x106d0;
    }
bb0x10744:
    puts(o0);
    return 0;
}

