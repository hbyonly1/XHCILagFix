/* USBXHCIWakeup userspace control. No USB endpoint, disk or trace access. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOOL_VERSION "0.2.0"

static int parse_id(const char *text, uint64_t *value) {
    if (!text || !*text || *text == '+' || *text == '-' || *text == ' ' || *text == '\t') return -1;
    int base = 10;
    const char *digits = text;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) { base = 16; digits += 2; }
    if (!*digits) return -1;
    for (const char *p = digits; *p; ++p) {
        int allowed = (*p >= '0' && *p <= '9') ||
            (base == 16 && ((*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')));
        if (!allowed) return -1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, base);
    if (errno || !end || *end || !parsed || parsed > INT64_MAX) return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static CFMutableDictionaryRef dictionary(void) {
    return CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}

static void set_number(CFMutableDictionaryRef dict, CFStringRef key, int64_t value) {
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &value);
    if (number) { CFDictionarySetValue(dict, key, number); CFRelease(number); }
}

static CFMutableDictionaryRef command_dictionary(const char *command, int has_id, uint64_t id) {
    CFMutableDictionaryRef dict = dictionary();
    if (!dict) return NULL;
    CFStringRef string = CFStringCreateWithCString(kCFAllocatorDefault,
        !strcmp(command, "check") ? "status" : command, kCFStringEncodingUTF8);
    if (!string) { CFRelease(dict); return NULL; }
    CFDictionarySetValue(dict, CFSTR("WakeupCommand"), string);
    CFRelease(string);
    if (has_id) set_number(dict, CFSTR("ControllerRegistryID"), (int64_t)id);
    return dict;
}

static int write_plist(const char *path, CFDictionaryRef properties) {
    CFErrorRef error = NULL;
    CFDataRef data = CFPropertyListCreateData(kCFAllocatorDefault, properties,
        kCFPropertyListXMLFormat_v1_0, 0, &error);
    if (error) CFRelease(error);
    if (!data) { fprintf(stderr, "Cannot serialize status plist.\n"); return -1; }
    FILE *stream = fopen(path, "wb");
    if (!stream) { perror(path); CFRelease(data); return -1; }
    CFIndex size = CFDataGetLength(data);
    int ok = size >= 0 && fwrite(CFDataGetBytePtr(data), 1, (size_t)size, stream) == (size_t)size;
    CFRelease(data);
    if (fclose(stream)) ok = 0;
    if (!ok) fprintf(stderr, "Cannot finish writing %s.\n", path);
    return ok ? 0 : -1;
}

static void string_to_stream(FILE *stream, CFStringRef string) {
    CFIndex maximum = CFStringGetMaximumSizeForEncoding(CFStringGetLength(string), kCFStringEncodingUTF8);
    if (maximum < 0 || maximum == LONG_MAX) { fputs("<string conversion failed>", stream); return; }
    char *buffer = malloc((size_t)maximum + 1);
    if (!buffer) { fputs("<out of memory>", stream); return; }
    if (CFStringGetCString(string, buffer, maximum + 1, kCFStringEncodingUTF8)) fputs(buffer, stream);
    else fputs("<string conversion failed>", stream);
    free(buffer);
}

static void indent(FILE *stream, unsigned depth) { for (unsigned i = 0; i < depth; ++i) fputs("  ", stream); }

static void text_value(FILE *stream, CFTypeRef value, unsigned depth) {
    if (depth > 32) { fputs("<nesting limit; see full plist>\n", stream); return; }
    CFTypeID type = CFGetTypeID(value);
    if (type == CFDictionaryGetTypeID()) {
        CFDictionaryRef dict = value;
        CFIndex count = CFDictionaryGetCount(dict);
        const void **keys = calloc((size_t)count + 1, sizeof(void *));
        const void **values = calloc((size_t)count + 1, sizeof(void *));
        if (!keys || !values) { free(keys); free(values); fputs("<out of memory>\n", stream); return; }
        CFDictionaryGetKeysAndValues(dict, keys, values);
        fputs("{\n", stream);
        for (CFIndex i = 0; i < count; ++i) {
            indent(stream, depth + 1);
            CFStringRef key = CFCopyDescription(keys[i]);
            if (key) { string_to_stream(stream, key); CFRelease(key); }
            fputs(" = ", stream);
            text_value(stream, values[i], depth + 1);
        }
        indent(stream, depth); fputs("}\n", stream);
        free(keys); free(values);
    } else if (type == CFArrayGetTypeID()) {
        CFArrayRef array = value;
        fputs("[\n", stream);
        for (CFIndex i = 0; i < CFArrayGetCount(array); ++i) {
            indent(stream, depth + 1); fprintf(stream, "[%ld] ", (long)i);
            text_value(stream, CFArrayGetValueAtIndex(array, i), depth + 1);
        }
        indent(stream, depth); fputs("]\n", stream);
    } else if (type == CFBooleanGetTypeID()) fputs(CFBooleanGetValue(value) ? "true\n" : "false\n", stream);
    else if (type == CFStringGetTypeID()) { string_to_stream(stream, value); fputc('\n', stream); }
    else {
        CFStringRef description = CFCopyDescription(value);
        if (description) { string_to_stream(stream, description); CFRelease(description); }
        fputc('\n', stream);
    }
}

static int write_text(const char *path, CFDictionaryRef properties) {
    FILE *stream = fopen(path, "w");
    if (!stream) { perror(path); return -1; }
    fputs("USBXHCIWakeup control/status " TOOL_VERSION "\n"
          "This is a status snapshot, not a throughput test. Calls/progress alone do not establish causality.\n"
          "Requested 500 us polling is a target interval, not a guaranteed scheduler deadline.\n", stream);
    text_value(stream, properties, 0);
    int ok = !ferror(stream);
    if (fclose(stream)) ok = 0;
    return ok ? 0 : -1;
}

static CFMutableDictionaryRef load_mock(const char *path) {
    FILE *stream = fopen(path, "rb");
    if (!stream) { perror(path); return NULL; }
    if (fseek(stream, 0, SEEK_END)) { fclose(stream); return NULL; }
    long length = ftell(stream);
    if (length < 0 || length > 16 * 1024 * 1024 || fseek(stream, 0, SEEK_SET)) { fclose(stream); return NULL; }
    unsigned char *bytes = malloc((size_t)length + 1);
    if (!bytes) { fclose(stream); return NULL; }
    int ok = fread(bytes, 1, (size_t)length, stream) == (size_t)length;
    fclose(stream);
    CFDataRef data = ok ? CFDataCreate(kCFAllocatorDefault, bytes, length) : NULL;
    free(bytes);
    if (!data) return NULL;
    CFPropertyListRef list = CFPropertyListCreateWithData(kCFAllocatorDefault, data,
        kCFPropertyListMutableContainersAndLeaves, NULL, NULL);
    CFRelease(data);
    if (!list) return NULL;
    if (CFGetTypeID(list) != CFDictionaryGetTypeID()) { CFRelease(list); return NULL; }
    return (CFMutableDictionaryRef)list;
}

static int selftest(void) {
    uint64_t id = 0;
    const char *bad[] = {"", "0", "-1", "+1", " 1", "1 ", "0x", "0x0", "1z", "1.0", "9223372036854775808", "18446744073709551616"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        if (!parse_id(bad[i], &id)) { fprintf(stderr, "ID validation failed.\n"); return 1; }
    if (parse_id("0123", &id) || id != 123 || parse_id("0x123", &id) || id != 291) return 1;
    CFMutableDictionaryRef check = command_dictionary("check", 0, 0);
    CFMutableDictionaryRef start = command_dictionary("start", 1, 291);
    CFTypeRef command = check ? CFDictionaryGetValue(check, CFSTR("WakeupCommand")) : NULL;
    CFTypeRef number = start ? CFDictionaryGetValue(start, CFSTR("ControllerRegistryID")) : NULL;
    int64_t numeric = 0;
    int ok = command && CFEqual(command, CFSTR("status")) && number &&
        CFNumberGetValue(number, kCFNumberSInt64Type, &numeric) && numeric == 291 &&
        !CFDictionaryContainsKey(check, CFSTR("ControllerRegistryID"));
    if (check) CFRelease(check);
    if (start) CFRelease(start);
    if (!ok) return 1;
    puts("PASS: command dictionaries and RegistryID validation; no IOKit access.");
    return 0;
}

static void usage(void) {
    fputs("Usage: wakeupctl-x86_64 --plist FILE --text FILE check|start|stop [ControllerRegistryID]\n"
          "       wakeupctl-x86_64 --selftest\n"
          "Offline test only: --mock-plist FILE (check only; never opens IOKit)\n", stderr);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--selftest")) return selftest();
    const char *plist = NULL, *text = NULL, *mock = NULL, *command = NULL;
    uint64_t id = 0;
    int has_id = 0;
    for (int i = 1; i < argc; ++i) {
        if ((!strcmp(argv[i], "--plist") || !strcmp(argv[i], "--text") || !strcmp(argv[i], "--mock-plist")) && i + 1 < argc) {
            const char *option = argv[i++];
            const char **target = !strcmp(option, "--plist") ? &plist : !strcmp(option, "--text") ? &text : &mock;
            if (*target) { usage(); return 64; }
            *target = argv[i];
        } else if (!command && (!strcmp(argv[i], "check") || !strcmp(argv[i], "start") || !strcmp(argv[i], "stop"))) command = argv[i];
        else if (command && strcmp(command, "check") && !has_id && !parse_id(argv[i], &id)) has_id = 1;
        else { usage(); return 64; }
    }
    if (!command || !plist || !text || !strcmp(plist, text) || (mock && strcmp(command, "check"))) { usage(); return 64; }
    CFMutableDictionaryRef properties = NULL;
    kern_return_t command_result = KERN_FAILURE;
    int result = 0, available = 0;
    if (mock) {
        properties = load_mock(mock);
        if (!properties) { fprintf(stderr, "Invalid mock plist.\n"); return 65; }
        available = 1;
        command_result = KERN_SUCCESS;
    } else {
        CFMutableDictionaryRef matching = IOServiceMatching("USBXHCIWakeupService");
        io_service_t service = matching ? IOServiceGetMatchingService(MACH_PORT_NULL, matching) : IO_OBJECT_NULL;
        if (!service) {
            /* The IOClass and registry name are intentionally distinct. */
            matching = IOServiceNameMatching("USBXHCIWakeup");
            service = matching ? IOServiceGetMatchingService(MACH_PORT_NULL, matching) : IO_OBJECT_NULL;
        }
        if (!service) { fprintf(stderr, "USBXHCIWakeup service unavailable.\n"); result = 2; }
        else {
            available = 1;
            CFMutableDictionaryRef request = command_dictionary(command, has_id, id);
            command_result = request ? IORegistryEntrySetCFProperties(service, request) : KERN_RESOURCE_SHORTAGE;
            if (request) CFRelease(request);
            if (command_result != KERN_SUCCESS) {
                fprintf(stderr, "%s command failed: 0x%08x; saving available status.\n", command, command_result);
                result = 3;
            }
            if (IORegistryEntryCreateCFProperties(service, &properties, kCFAllocatorDefault, 0) != KERN_SUCCESS) {
                fprintf(stderr, "Cannot read service properties.\n");
                result = 4;
            }
            IOObjectRelease(service);
        }
    }
    if (!properties) properties = dictionary();
    if (!properties) return 70;
    CFDictionarySetValue(properties, CFSTR("ControlToolVersion"), CFSTR(TOOL_VERSION));
    CFDictionarySetValue(properties, CFSTR("ControlToolServiceAvailable"), available ? kCFBooleanTrue : kCFBooleanFalse);
    CFDictionarySetValue(properties, CFSTR("ControlToolMock"), mock ? kCFBooleanTrue : kCFBooleanFalse);
    CFStringRef operation = CFStringCreateWithCString(kCFAllocatorDefault, command, kCFStringEncodingUTF8);
    if (operation) { CFDictionarySetValue(properties, CFSTR("ControlToolCommand"), operation); CFRelease(operation); }
    set_number(properties, CFSTR("ControlToolCommandResult"), (int64_t)(uint32_t)command_result);
    if (has_id) set_number(properties, CFSTR("ControlToolControllerRegistryID"), (int64_t)id);
    /* Never remove the driver's properties, including the complete Controllers array. */
    if (write_plist(plist, properties)) result = 5;
    if (write_text(text, properties)) result = 5;
    CFRelease(properties);
    return result;
}
