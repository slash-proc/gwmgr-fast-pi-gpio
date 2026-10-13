#define main gdb_server_main
#include "../src/gdb_swd.c"
#undef main
#undef NDEBUG
#include <assert.h>

static void query(const char *request, char *response, size_t capacity)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct gdb_target target = {0};
    assert(handle_packet(&target, sockets[0], (const uint8_t *)request, strlen(request)));
    ssize_t count = recv(sockets[1], response, capacity - 1U, 0);
    assert(count > 0);
    response[count] = '\0';
    close(sockets[0]);
    close(sockets[1]);
}

int main(void)
{
    char response[2048];
    unsigned index = 0U;
    assert(register_index((const uint8_t *)"19", 2U, &index) && index == 16U);
    assert(register_index((const uint8_t *)"10", 2U, &index) && index == 16U);
    assert(!register_index((const uint8_t *)"18", 2U, &index));
    query("qSupported", response, sizeof(response));
    assert(strstr(response, "qXfer:features:read+") != NULL);
    query("qXfer:features:read:target.xml:0,fff", response, sizeof(response));
    assert(strncmp(response, "$l<?xml", 7U) == 0);
    assert(strstr(response, "org.gnu.gdb.arm.m-profile") != NULL);
    assert(strstr(response, "regnum=\"25\"") != NULL);
    query("qXfer:features:read:target.xml:0,10", response, sizeof(response));
    assert(strncmp(response, "$m", 2U) == 0);
    query("qXfer:features:read:target.xml:ffff,10", response, sizeof(response));
    assert(strncmp(response, "$l#", 3U) == 0);
    puts("GDB target-description and register mapping checks passed");
    return 0;
}
