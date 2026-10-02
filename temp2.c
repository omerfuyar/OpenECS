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

int main()
{
    char currentLine[MAX_LINE_LETTERS];
    int currentLineLength;

    char selectedLines[MAX_LINES][MAX_LINE_LETTERS];
    int selectedLineCount = 0;

    while ((currentLineLength = mgetline(currentLine, MAX_LINE_LETTERS)) > 0)
    {
        if (currentLineLength > LINE_LIMIT)
        {
            copy(selectedLines[selectedLineCount], currentLine);
            selectedLineCount++;
        }
    }

    printf("Lines longer than %d:\n", LINE_LIMIT);
    for (int i = 0; i < selectedLineCount; i++)
    {
        printf("%s\n", selectedLines[i]);
    }

    return 0;
}