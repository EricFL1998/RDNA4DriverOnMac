/* One-shot console input diagnostics. No autologin or persistent settings. */
#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_session(void) {
    CFDictionaryRef session = CGSessionCopyCurrentDictionary();
    if (!session) {
        puts("session=unavailable");
        return;
    }
    CFStringRef keys[] = { CFSTR("kCGSSessionUserIDKey"), CFSTR("kCGSessionLoginDoneKey") };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        CFTypeRef value = CFDictionaryGetValue(session, keys[i]);
        char key[64], text[128] = "missing";
        CFStringGetCString(keys[i], key, sizeof(key), kCFStringEncodingUTF8);
        if (value) CFShow(value);
        if (value && CFGetTypeID(value) == CFNumberGetTypeID()) {
            int n = 0;
            CFNumberGetValue(value, kCFNumberIntType, &n);
            snprintf(text, sizeof(text), "%d", n);
        }
        printf("%s=%s\n", key, text);
    }
    CFRelease(session);
}

static int post_key(CGKeyCode key) {
    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    if (!source) return 1;
    CGEventRef down = CGEventCreateKeyboardEvent(source, key, true);
    CGEventRef up = CGEventCreateKeyboardEvent(source, key, false);
    if (!down || !up) return 2;
    CGEventPost(kCGHIDEventTap, down);
    usleep(20000);
    CGEventPost(kCGHIDEventTap, up);
    CFRelease(down);
    CFRelease(up);
    CFRelease(source);
    return 0;
}

static const CGKeyCode keymap[36] = {
    0, 11, 8, 2, 14, 3, 5, 4, 34, 38, 40, 37, 46, 45, 31, 35, 12, 15,
    1, 17, 32, 9, 13, 7, 16, 6, 18, 19, 20, 21, 23, 22, 26, 28, 25, 29
};

int main(int argc, char **argv) {
    print_session();
    printf("uid=%d post-event-access=%d\n", getuid(), CGPreflightPostEventAccess() ? 1 : 0);
    if (argc == 2 && strcmp(argv[1], "--probe") == 0) {
        int rc = post_key(53); /* Escape: wake/focus without typing or submitting. */
        printf("escape-posted=%d\n", rc);
        return rc;
    }
    if (argc == 2 && strcmp(argv[1], "--login") == 0) {
        char password[128];
        if (!fgets(password, sizeof(password), stdin)) return 3;
        password[strcspn(password, "\r\n")] = '\0';
        for (const unsigned char *p = (unsigned char *)password; *p; p++) {
            if (!isalnum(*p)) {
                fputs("only alphanumeric passwords are supported by this diagnostic\n", stderr);
                return 4;
            }
            int index = isdigit(*p) ? 26 + *p - '0' : tolower(*p) - 'a';
            if (post_key(keymap[index]) != 0) return 5;
        }
        memset(password, 0, sizeof(password));
        usleep(100000);
        int rc = post_key(36); /* Return */
        printf("password-and-return-posted=%d\n", rc);
        return rc;
    }
    fprintf(stderr, "usage: %s --probe | --login < password\n", argv[0]);
    return 64;
}
