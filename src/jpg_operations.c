#ifdef _WIN32
#   define _CRT_SECURE_NO_DEPRECATE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "jpg_operations.h"
#include "csv_lookup.h"
#define HEX_MULTIPLIER        16
#define HEX_MULTIPLIER_POW_2 256
#define JPG_EXIF_TAGS_FILEPATH "./jpg_exif_tags.csv"

static char byte_alignment[3];
static int byte_alignment_offset;
static int next_ifd_offset;


typedef struct {
    int *tags;
    size_t tags_count;
    size_t capacity;
} Exif_Tag_Array;


#define tags_append(arr, tag)\
    do {\
        if (arr.tags_count >= arr.capacity) {\
            if (arr.capacity == 0) arr.capacity = 256;\
            else arr.capacity *= 2;\
            arr.tags = realloc(arr.tags, arr.capacity * sizeof(*arr.tags));\
        }\
        arr.tags[arr.tags_count++] = tag;\
    } while (0)


static bool _tag_has_occured(Exif_Tag_Array arr, int tag)
{
    for (size_t i = 0; i < arr.tags_count; i++) {
        if (arr.tags[i] == tag) return true;
    }
    return false;
}


static int _get_nth_power(int number, int power)
{
    if (power == 0)
        return 1;

    else if (power == 1)
        return number;

    int original_number = number;
    for (int i = power; i > 1; i--) {
        number *= original_number;
    }
    return number;
}


static int _get_single_digit_from_byte(int first_or_second, int byte)
{
    if (first_or_second == 1) {
        return (byte - (byte % HEX_MULTIPLIER)) / HEX_MULTIPLIER;
    }

    else if (first_or_second == 2) {
        return (byte % HEX_MULTIPLIER);
    }

    return -1;
}


static int _read_n_byte_int(FILE *image_file, int n)
{
    int c;
    int total = 0;

    if (strcmp(byte_alignment, "MM") == 0) {
        for (int i = n; i > 0; i--) {
            c = fgetc(image_file);

            total += _get_single_digit_from_byte(1, c) * _get_nth_power(HEX_MULTIPLIER, i*2-1);
            total += _get_single_digit_from_byte(2, c) * _get_nth_power(HEX_MULTIPLIER, i*2-2);
        }

        return total;
    }

    for (int i = 0; i < n; i++) {
        c = fgetc(image_file);

        total += _get_single_digit_from_byte(1, c) * _get_nth_power(HEX_MULTIPLIER, i*2+1);
        total += _get_single_digit_from_byte(2, c) * _get_nth_power(HEX_MULTIPLIER, i*2);
    }

    return total;
}


// data values sometimes only store data in the first n out of the 4
// available bytes, so reading all 4 can result in incorrect data
static int _read_4_byte_data_value(FILE *image_file)
{
    if (strcmp(byte_alignment, "II") == 0) {
        return _read_n_byte_int(image_file, 4);
    }

    int val, val2;

    val = _read_n_byte_int(image_file, 2);
    val2 = _read_n_byte_int(image_file, 2);

    if (val > 0 && val2 > 0) {
        fseek(image_file, -4, SEEK_CUR);
        return _read_n_byte_int(image_file, 4);
    }

    if (val == 0)
        return val2;

    return val;
}


static void _goto_file_offset(FILE *file, int offset)
{
    fseek(file, offset + byte_alignment_offset, SEEK_SET);
}


static int _get_string_offset_from_start_of_file(FILE *image_file, char *str)
{
    fseek(image_file, 0, SEEK_SET);

    int offset = 0;
    int str_char1 = str[0];
    int str_len = strlen(str);
    int file_char;

    while ((file_char = fgetc(image_file)) != EOF) {
        offset++;

        if (file_char == str_char1) {
            for (int i = 1; i < str_len; i++) {
                file_char = fgetc(image_file);
                offset++;

                if (file_char != str[i]) {
                    fseek(image_file, -i, SEEK_CUR);
                    offset -= i;
                    break;
                }

                else if (i == str_len-1) {
                    return offset;
                }
            }
        }
    }

    return -1;
}


static void _read_tiff_header(FILE *image_file)
{
    if ((byte_alignment_offset = _get_string_offset_from_start_of_file(image_file, "Exif")) == -1) {
        fprintf(stderr, "[ERROR] could not find image header\n%s : %d\n", __FILE__, __LINE__);
    }

    fseek(image_file, 2, SEEK_CUR);   // skip 2x 0x00
    if (fgetc(image_file) == 'M') {   // Motorola
        strcpy(byte_alignment, "MM");
    }
    else {
        strcpy(byte_alignment, "II"); // Intel
    }

    byte_alignment_offset += 2;       // account for the skipped null bytes

    fseek(image_file, 3, SEEK_CUR);   // skip the next I or M and 0x002A or 0x2A00
    next_ifd_offset = _read_n_byte_int(image_file, 4);
    printf("#next ifd offset: %d\n", next_ifd_offset);

}


// can be used with `data format` 5, 10; length is expected to be 8
static void _print_fract_value_by_offset(FILE *image_file, int offset)
{
    long old_file_pos = ftell(image_file);
    _goto_file_offset(image_file, offset);

    printf("%d/", _read_n_byte_int(image_file, 4));
    printf("%d\n", _read_n_byte_int(image_file, 4));

    fseek(image_file, old_file_pos, SEEK_SET);
}


static void _print_ascii_string_by_offset(FILE *image_file, int offset, int string_length)
{
    int old_file_pos = ftell(image_file);
    char letter;

    _goto_file_offset(image_file, offset);

    for (int i = 0; i < string_length; i++) {
        letter = fgetc(image_file);
        putchar(letter);
    }
    putchar('\n');

    fseek(image_file, old_file_pos, SEEK_SET);
}


static void _print_value_by_offset(FILE *image_file, int offset, int total_components_length, int n_components)
{
    long old_file_pos = ftell(image_file);
    _goto_file_offset(image_file, offset);

    int single_component_length = total_components_length/n_components;

    for (int i = 0; i < n_components; i++) {
        if (i == n_components - 1) {
            printf("%d\n", _read_n_byte_int(image_file, single_component_length));
            break;
        }
        printf("%d, ", _read_n_byte_int(image_file, single_component_length));
    }

    fseek(image_file, old_file_pos, SEEK_SET);
}


static int _get_total_components_length(int format_value, int components_count)
{
    switch (format_value) {
        case 1: case 2: case 6: case 7:
            return 1 * components_count;

        case 3: case 8:
            return 2 * components_count;

        case 4: case 9: case 11:
            return 4 * components_count;

        case 5: case 10: case 12:
            return 8 * components_count;

        default:
            fprintf(stderr, "\n[ERROR] Unrecognized format value (%d)\n%s : %d\n", format_value, __FILE__, __LINE__);
            exit(EXIT_FAILURE);
    }
}


static void _indent_result(size_t keyword_length)
{
    keyword_length++; // count in a colon

    if (keyword_length < 5)
        printf("\t\t\t\t\t");

    else if (keyword_length < 8)
        printf("\t\t\t\t");

    else if (keyword_length < 16)
        printf("\t\t\t");

    else if (keyword_length < 20)
        printf("\t\t");

    else if (keyword_length < 28)
        putchar('\t');
}


static bool _data_format_is_fractional(int data_format)
{
    if (data_format == 5 || data_format == 10) return true;
    return false;
}


static bool _data_format_is_int(int data_format)
{
    if (data_format == 1 || data_format == 3 || data_format == 4 || data_format == 6 || \
        data_format == 7 || data_format == 8 || data_format == 9) {
        return true;
    }
    return false;
}


static void _print_resolution_unit(FILE *image_file)
{
    int c, i;

    for (i = 0; i < 4; i++) {
        c = _read_n_byte_int(image_file, 1);

        if (c == 0)
            continue;

        if (c > 3)
            continue;

        fseek(image_file, 3-i, SEEK_CUR);

        switch (c) {
            case 1:
                printf("None (%d)\n", c);
                break;

            case 2:
                printf("inches (%d)\n", c);
                break;

            case 3:
                printf("cm (%d)\n", c);
                break;
        }
        return;
    }
}


static void _print_ycbcr_positioning(FILE *image_file)
{
    int c, i;

    for (i = 0; i < 4; i++) {
        c = _read_n_byte_int(image_file, 1);

        if (c != 1 && c != 2)
            continue;

        fseek(image_file, 3-i, SEEK_CUR);

        switch (c) {
            case 1:
                printf("centered (%d)\n", c);
                return;

            case 2:
                printf("co-sited (%d)\n", c);
                return;
        }
    }
}


static void _print_custom_rendered(FILE *image_file)
{
        int c, i;

        for (i = 0; i < 4; i++) {
            c = _read_n_byte_int(image_file, 1);

            if (c > 8)
                continue;

            fseek(image_file, 3-i, SEEK_CUR);
            switch (c) {
                case 0:
                    printf("normal (%d)\n", i);
                    return;

                case 1:
                    printf("custom (%d)\n", i);
                    return;

                case 2:
                    printf("HDR - no original saved (%d)\n", i);
                    return;

                case 3:
                    printf("HDR - original saved (%d)\n", i);
                    return;

                case 4:
                    printf("original - for HDR (%d)\n", i);
                    return;

                case 5:
                    printf("panorama (%d)\n", i);
                    return;

                case 6:
                    printf("portrait HDR (%d)\n", i);
                    return;

                case 7:
                    printf("portrait HDR (%d)\n", i);
                    return;

                case 8:
                    printf("portrait (%d)\n", i);
                    return;
            }
        }
}


static void _print_exposure_mode(FILE *image_file)
{
    int c, i;

    for (i = 0; i < 4; i++) {
        c = _read_n_byte_int(image_file, 1);

        fseek(image_file, 3-i, SEEK_CUR);
        switch (c) {
            case 0:
                printf("auto (%d)\n", i);
                return;

            case 1:
                printf("manual (%d)\n", i);
                return;

            case 2:
                printf("auto bracket (%d)\n", i);
                return;
        }
    }
}


// expects that the file pointer is right before the ifd starts
// that is, before the number of directory entries
// returns the offset to the next IFD
static int _print_ifd_entry_data(FILE *image_file, FILE *csv_fp)
{
    static Exif_Tag_Array exif_tags = {0};
    int tag_number, data_format, n_components, data_offset, total_components_length;
    int n_entries = _read_n_byte_int(image_file, 2);
    //printf("#number of entries: %d\n", n_entries);

    for (int i = 0; i < n_entries; i++) {
        tag_number = _read_n_byte_int(image_file, 2);
        data_format = _read_n_byte_int(image_file, 2);
        n_components = _read_n_byte_int(image_file, 4);

        //printf("\ntag number: %x\n", tag_number);
        //printf("data format: %d\n", data_format);
        //printf("n. components: %d\n", n_components);

        if (!_tag_has_occured(exif_tags, tag_number)) tags_append(exif_tags, tag_number);
        else {
            fseek(image_file, 4, SEEK_CUR);
            continue;
        }

        char *tag_string = csv_get_string_by_value(csv_fp, tag_number);
        printf("%s: ", tag_string);
        _indent_result(strlen(tag_string));
        free(tag_string);

        if (tag_number == 0x0128) {
            // resolution unit is sometimes stored in a very unexpected way, this should combat that
            _print_resolution_unit(image_file);
            continue;
        }
        else if (tag_number == 0x0213) {
            _print_ycbcr_positioning(image_file);
            continue;
        }
        else if (tag_number == 0xa401) {
            _print_custom_rendered(image_file);
            continue;
        }
        else if (tag_number == 0xa402) {
            _print_exposure_mode(image_file);
            continue;
        }

        if ((total_components_length = _get_total_components_length(data_format, n_components)) > 4) {
            data_offset = _read_n_byte_int(image_file, 4);
        }
        else {
            switch(data_format) {
                case 2:
                    _print_ascii_string_by_offset(image_file, ftell(image_file)-byte_alignment_offset, 4);
                    break;

                default:
                    printf("%d\n", _read_4_byte_data_value(image_file));
                    break;
            }
            continue;
        }

        if (_data_format_is_fractional(data_format)) {
            _print_fract_value_by_offset(image_file, data_offset);
        }
        else if (_data_format_is_int(data_format) && total_components_length > 4) {
            _print_value_by_offset(image_file, data_offset, total_components_length, n_components);
        }
        else if (n_components > 4 && data_format == 2) {
            _print_ascii_string_by_offset(image_file, data_offset, n_components);
        }
    }

    next_ifd_offset = _read_4_byte_data_value(image_file);
    if (next_ifd_offset == 0) {
        free(exif_tags.tags);
        return 0;
    }

    _goto_file_offset(image_file, next_ifd_offset);
    _print_ifd_entry_data(image_file, csv_fp);

    return 0;
}


void jpg_print_exif_data(FILE *image_file)
{
    _read_tiff_header(image_file);

    printf("byte alignment:\t\t\t%s", byte_alignment);
    strcmp(byte_alignment, "MM") == 0 ? printf(" (Big Endian/Motorola)\n") : printf(" (Little Endian/Intel)\n");

    FILE *csv_fp = fopen(JPG_EXIF_TAGS_FILEPATH, "rb");
    _print_ifd_entry_data(image_file, csv_fp);
    fclose(csv_fp);
}

