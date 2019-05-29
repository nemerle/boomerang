int main(int argc, char *argv[]);


/** address: 0x10000408 */
int main(int argc, char *argv[])
{
    if ((unsigned int)argc > 7) {
lab_4:
        puts("Other!");
        break;
    }
    switch(argc) {
    case 0:
    case 1:
        goto lab_4;
    case 7:
        puts("Seven!");
        break;
    case 2:
        puts("Two!");
        break;
    case 3:
        puts("Three!");
        break;
    case 4:
        puts("Four!");
        break;
    case 5:
        puts("Five!");
        break;
    case 6:
        puts("Six!");
        break;
    }
    return 0;
}

