#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "docopt.h"

struct Command {
    const char *name;
    bool value;
};

struct Argument {
    const char *name;
    const char *value;
    const char *array[ARG_MAX];
};

struct Option {
    const char *oshort;
    const char *olong;
    bool argcount;
    bool value;
    const char *argument;
};

struct Elements {
    int n_commands;
    int n_arguments;
    int n_options;
    struct Command *commands;
    struct Argument *arguments;
    struct Option *options;
};


/*
 * Tokens object
 */

struct Tokens {
    int argc;
    char **argv;
    int i;
    char *current;
};

const char usage_pattern[] =
        "Usage: virtualxt [options]";

struct Tokens tokens_new(int argc, char **argv) {
    struct Tokens ts;
    ts.argc = argc;
    ts.argv = argv;
    ts.i = 0;
    ts.current = argv[0];
    return ts;
}

struct Tokens *tokens_move(struct Tokens *ts) {
    if (ts->i < ts->argc) {
        ts->current = ts->argv[++ts->i];
    }
    if (ts->i == ts->argc) {
        ts->current = NULL;
    }
    return ts;
}


/*
 * ARGV parsing functions
 */

int parse_doubledash(struct Tokens *ts, struct Elements *elements) {
    /*
    int n_commands = elements->n_commands;
    int n_arguments = elements->n_arguments;
    Command *commands = elements->commands;
    Argument *arguments = elements->arguments;

    not implemented yet
    return parsed + [Argument(None, v) for v in tokens]
    */
    return 0;
}

int parse_long(struct Tokens *ts, struct Elements *elements) {
    int i;
    int len_prefix;
    int n_options = elements->n_options;
    char *eq = strchr(ts->current, '=');
    struct Option *option;
    struct Option *options = elements->options;

    len_prefix = (eq - (ts->current)) / sizeof(char);
    for (i = 0; i < n_options; i++) {
        option = &options[i];
        if (!strncmp(ts->current, option->olong, len_prefix))
            break;
    }
    if (i == n_options) {
        /* TODO: %s is not a unique prefix */
        fprintf(stderr, "%s is not recognized\n", ts->current);
        return 1;
    }
    tokens_move(ts);
    if (option->argcount) {
        if (eq == NULL) {
            if (ts->current == NULL) {
                fprintf(stderr, "%s requires argument\n", option->olong);
                return 1;
            }
            option->argument = ts->current;
            tokens_move(ts);
        } else {
            option->argument = eq + 1;
        }
    } else {
        if (eq != NULL) {
            fprintf(stderr, "%s must not have an argument\n", option->olong);
            return 1;
        }
        option->value = true;
    }
    return 0;
}

int parse_shorts(struct Tokens *ts, struct Elements *elements) {
    char *raw;
    int i;
    int n_options = elements->n_options;
    struct Option *option;
    struct Option *options = elements->options;

    raw = &ts->current[1];
    tokens_move(ts);
    while (raw[0] != '\0') {
        for (i = 0; i < n_options; i++) {
            option = &options[i];
            if (option->oshort != NULL && option->oshort[1] == raw[0])
                break;
        }
        if (i == n_options) {
            /* TODO -%s is specified ambiguously %d times */
            fprintf(stderr, "-%c is not recognized\n", raw[0]);
            return EXIT_FAILURE;
        }
        raw++;
        if (!option->argcount) {
            option->value = true;
        } else {
            if (raw[0] == '\0') {
                if (ts->current == NULL) {
                    fprintf(stderr, "%s requires argument\n", option->oshort);
                    return EXIT_FAILURE;
                }
                raw = ts->current;
                tokens_move(ts);
            }
            option->argument = raw;
            break;
        }
    }
    return EXIT_SUCCESS;
}

int parse_argcmd(struct Tokens *ts, struct Elements *elements) {
    int i;
    int n_commands = elements->n_commands;
    /* int n_arguments = elements->n_arguments; */
    struct Command *command;
    struct Command *commands = elements->commands;
    /* Argument *arguments = elements->arguments; */

    for (i = 0; i < n_commands; i++) {
        command = &commands[i];
        if (strcmp(command->name, ts->current) == 0) {
            command->value = true;
            tokens_move(ts);
            return EXIT_SUCCESS;
        }
    }
    /* not implemented yet, just skip for now
       parsed.append(Argument(None, tokens.move())) */
    /*
    fprintf(stderr, "! argument '%s' has been ignored\n", ts->current);
    fprintf(stderr, "  '");
    for (i=0; i<ts->argc ; i++)
        fprintf(stderr, "%s ", ts->argv[i]);
    fprintf(stderr, "'\n");
    */
    tokens_move(ts);
    return EXIT_SUCCESS;
}

int parse_args(struct Tokens *ts, struct Elements *elements) {
    int ret = EXIT_FAILURE;

    while (ts->current != NULL) {
        if (strcmp(ts->current, "--") == 0) {
            ret = parse_doubledash(ts, elements);
            if (ret == EXIT_FAILURE) break;
        } else if (ts->current[0] == '-' && ts->current[1] == '-') {
            ret = parse_long(ts, elements);
        } else if (ts->current[0] == '-' && ts->current[1] != '\0') {
            ret = parse_shorts(ts, elements);
        } else
            ret = parse_argcmd(ts, elements);
        if (ret) return ret;
    }
    return ret;
}

int elems_to_args(struct Elements *elements, struct DocoptArgs *args,
                     const bool help, const char *version) {
    struct Command *command;
    struct Argument *argument;
    struct Option *option;
    int i, j;

    /* fix gcc-related compiler warnings (unused) */
    (void) command;
    (void) argument;

    /* options */
    for (i = 0; i < elements->n_options; i++) {
        option = &elements->options[i];
        if (help && option->value && strcmp(option->olong, "--help") == 0) {
            for (j = 0; j < 19; j++)
                puts(args->help_message[j]);
            return EXIT_FAILURE;
        } else if (version && option->value &&
                   strcmp(option->olong, "--version") == 0) {
            puts(version);
            return EXIT_FAILURE;
        } else if (strcmp(option->olong, "--clean") == 0) {
            args->clean = option->value;
        } else if (strcmp(option->olong, "--edit") == 0) {
            args->edit = option->value;
        } else if (strcmp(option->olong, "--halt") == 0) {
            args->halt = option->value;
        } else if (strcmp(option->olong, "--hdboot") == 0) {
            args->hdboot = option->value;
        } else if (strcmp(option->olong, "--help") == 0) {
            args->help = option->value;
        } else if (strcmp(option->olong, "--locate") == 0) {
            args->locate = option->value;
        } else if (strcmp(option->olong, "--mute") == 0) {
            args->mute = option->value;
        } else if (strcmp(option->olong, "--no-activity") == 0) {
            args->no_activity = option->value;
        } else if (strcmp(option->olong, "--full-screen") == 0) {
            args->full_screen = option->value;
        } else if (strcmp(option->olong, "--v20") == 0) {
            args->v20 = option->value;
        } else if (strcmp(option->olong, "--version") == 0) {
            args->version = option->value;
        } else if (strcmp(option->olong, "--config") == 0) {
            if (option->argument) {
                args->config = (char *) option->argument;
            }
        } else if (strcmp(option->olong, "--floppy") == 0) {
            if (option->argument) {
                args->floppy = (char *) option->argument;
            }
        } else if (strcmp(option->olong, "--frequency") == 0) {
            if (option->argument) {
                args->frequency = (char *) option->argument;
            }
        } else if (strcmp(option->olong, "--harddrive") == 0) {
            if (option->argument) {
                args->harddrive = (char *) option->argument;
            }
        } else if (strcmp(option->olong, "--rifs") == 0) {
            if (option->argument) {
                args->rifs = (char *) option->argument;
            }
        } else if (strcmp(option->olong, "--trace") == 0) {
            if (option->argument) {
                args->trace = (char *) option->argument;
            }
        }
    }
    /* commands */
    for (i = 0; i < elements->n_commands; i++) {
        command = &elements->commands[i];
        
    }
    /* arguments */
    for (i = 0; i < elements->n_arguments; i++) {
        argument = &elements->arguments[i];
        
    }
    return EXIT_SUCCESS;
}


/*
 * Main docopt function
 */

struct DocoptArgs docopt(int argc, char *argv[], const bool help, const char *version) {
    struct DocoptArgs args = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, NULL, (char *) "8.0", NULL, NULL,
        NULL,
            usage_pattern,
            { "Usage: virtualxt [options]",
              "",
              "Options:",
              "  -h --help               Show this screen.",
              "  -v --version            Display version.",
              "  --hdboot                Prefer booting from harddrive.",
              "  --halt                  Debug break on startup.",
              "  --mute                  Disable audio.",
              "  --no-activity           Disable disk activity indicator.",
              "  --full-screen           Start in full-screen mode.",
              "  --v20                   Enable NEC V20 CPU support.",
              "  --clean                 Remove config file and write a new default one.",
              "  --edit                  Open config file in system text editor.",
              "  --locate                Locate the configuration directory.",
              "  --rifs=PATH             Enable experimental RIFS support. (Shared folders)",
              "  --config=PATH           Set config directory.",
              "  --trace=FILE            Write CPU trace to file.",
              "  --frequency=MHZ         CPU frequency. [default: 8.0]",
              "  -a --floppy=FILE        Mount floppy image as drive A.",
              "  -c --harddrive=FILE     Mount harddrive image as drive C."}
    };
    struct Command commands[] = {NULL
    };
    struct Argument arguments[] = {NULL
    };
    struct Option options[] = {
        {NULL, "--clean", 0, 0, NULL},
        {NULL, "--edit", 0, 0, NULL},
        {NULL, "--halt", 0, 0, NULL},
        {NULL, "--hdboot", 0, 0, NULL},
        {"-h", "--help", 0, 0, NULL},
        {NULL, "--locate", 0, 0, NULL},
        {NULL, "--mute", 0, 0, NULL},
        {NULL, "--no-activity", 0, 0, NULL},
        {NULL, "--full-screen", 0, 0, NULL},
        {NULL, "--v20", 0, 0, NULL},
        {"-v", "--version", 0, 0, NULL},
        {NULL, "--config", 1, 0, NULL},
        {"-a", "--floppy", 1, 0, NULL},
        {NULL, "--frequency", 1, 0, NULL},
        {"-c", "--harddrive", 1, 0, NULL},
        {NULL, "--rifs", 1, 0, NULL},
        {NULL, "--trace", 1, 0, NULL}
    };
    struct Elements elements;
    int return_code = EXIT_SUCCESS;

    elements.n_commands = 0;
    elements.n_arguments = 0;
    elements.n_options = 16;
    elements.commands = commands;
    elements.arguments = arguments;
    elements.options = options;
    /*
    if (argc == 1) {
        argv[argc++] = "--help";
        argv[argc++] = NULL;
        return_code = EXIT_FAILURE;
    }
    */
    {
        struct Tokens ts = tokens_new(argc, argv);
        if (parse_args(&ts, &elements))
            exit(EXIT_FAILURE);
    }
    if (elems_to_args(&elements, &args, help, version))
        exit(return_code);
    return args;
}
