#include <stdio.h>

#define MAX_LINE_LETTERS 256
#define MAX_LINES 64
#define LINE_LIMIT 16

int mgetline(char line[], int maxline)
{
    int c, i;
    for (i = 0; i < maxline - 1 && (c = getchar()) != EOF && c != '\n'; ++i)
    {
        line[i] = c;
    }

    if (c == '\n')
    {
        line[i] = c;
        ++i;
    }

    line[i] = '\0';
    return i;
}

void copy(char to[], char from[])
{
    int i = 0;

    while ((to[i] = from[i]) != '\0')
    {
        ++i;
    }
}

int stringLength(char *str)
{
    int i = 0;

    while (str[i] != '\0')
    {
        i++;
    }

    return i;
}

int clear(char *str)
{
    int i = stringLength(str) - 1;

    while (i > 0 && str[i] != '\n' && str[i] != '\t' && str[i] != ' ')
    {
        i--;
    }

    return i;
}

int main()
{
    char currentLine[MAX_LINE_LETTERS];
    int currentLineLength;

    char selectedLines[MAX_LINES][MAX_LINE_LETTERS];
    int selectedLineCount = 0;

    while ((currentLineLength = mgetline(currentLine, MAX_LINE_LETTERS)) > 0)
    {
    }

    printf("Cleared lines:\n");
    for (int i = 0; i < selectedLineCount; i++)
    {
        printf("'%s'", selectedLines[i]);
    }

    return 0;
}