#include "Global.h"

#include <strings.h>

void ECSI_TextCopy(SHUSlice buffer, const char *text)
{
    SHU_AssertNullPointer(buffer.data);
    SHU_Assert(buffer.size > 0, "Buffer to copy text into is empty.");

    char *target = buffer.data;
    usz length = text == NULL ? 0 : strlen(text);

    if (length >= buffer.size)
    {
        length = buffer.size - 1;
    }

    memcpy(target, text == NULL ? "" : text, length);
    target[length] = '\0';
}

bool ECSI_TextEqualsIgnoreCase(const char *text, const char *other)
{
    SHU_AssertNullPointer(text);
    SHU_AssertNullPointer(other);

    return strcasecmp(text, other) == 0;
}

void ECSI_PathUserData(SHUSlice buffer, const char *name)
{
    SHU_AssertNullPointer(buffer.data);
    SHU_AssertNullPointer(name);

    const char *data = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");

    if (data != NULL && data[0] != '\0')
    {
        snprintf(buffer.data, buffer.size, "%s/%s", data, name);
    }
    else
    {
        snprintf(buffer.data, buffer.size, "%s/.local/share/%s", home == NULL ? "." : home, name);
    }
}

bool ECSI_TextStartsWith(const char *text, const char *prefix)
{
    SHU_AssertNullPointer(text);
    SHU_AssertNullPointer(prefix);

    return strncmp(text, prefix, strlen(prefix)) == 0;
}
