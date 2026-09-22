#include "wifi_field.h"

#include <string.h>

bool wifi_field_set(uint8_t *field, size_t size, const char *value)
{
    if (field == NULL || value == NULL) {
        return false;
    }

    /*
     * Not strlen(). This is the one place that has to survive a value longer than the
     * field it is going into, and past size + 1 the exact length stops being an answer
     * to anything: everything from there on is refused alike. Stopping there also puts
     * a bound on how far a source that lost its terminator can lead this.
     */
    size_t length = 0;
    while (length <= size && value[length] != '\0') {
        length++;
    }

    if (length > size) {
        return false;
    }

    memcpy(field, value, length);
    memset(field + length, 0, size - length);
    return true;
}
