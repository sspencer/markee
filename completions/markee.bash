# bash completion for markee

_markee() {
    local cur prev words cword
    if declare -F _init_completion >/dev/null 2>&1; then
        _init_completion || return
    else
        COMPREPLY=()
        cur="${COMP_WORDS[COMP_CWORD]}"
        prev="${COMP_WORDS[COMP_CWORD-1]}"
        words=("${COMP_WORDS[@]}")
        cword=$COMP_CWORD
    fi

    local subcommands="say notify gif timer clock alert draw clear info"
    local server_opts="-r --resolution -z --zoom --square --circle -s --shape -m --monochrome --mono -H --host -p --port --token --dpi --always-on-top --topmost --no-always-on-top --borderless --frameless --no-borderless --position --pos --corner --case --idle-dim --idle-brightness --demo --blank --mute --no-audio --headless --config --no-config --save-position -h --help"

    local positions="nw n ne w c e sw s se center top-left top top-right left right bottom-left bottom bottom-right"
    local colors="white warmwhite red green blue cyan magenta yellow amber orange purple pink black"
    local shapes="square circle"
    local cases="white charcoal orange retro"
    local sounds="ding chime bell"
    local fonts="4x6 5x7 6x8 8x8 prop"
    local fits="crop contain stretch"
    local transitions="fade dip slide-left slide-right slide-up slide-down wipe wipe-left wipe-up wipe-down split center-out blinds dissolve"
    local clock_formats="24h 24hm 12h 12hm"

    # Determine whether a client subcommand is active at position 1
    local subcmd=""
    if (( cword >= 2 )); then
        case "${words[1]}" in
            say|notify|gif|timer|clock|alert|draw|clear|info)
                subcmd="${words[1]}"
                ;;
        esac
    fi

    # Value completions for options that take an argument
    case "$prev" in
        -s|--shape)
            COMPREPLY=( $(compgen -W "$shapes" -- "$cur") )
            return
            ;;
        --position|--pos|--corner)
            COMPREPLY=( $(compgen -W "$positions" -- "$cur") )
            return
            ;;
        --case)
            COMPREPLY=( $(compgen -W "$cases" -- "$cur") )
            return
            ;;
        -m|--monochrome|--mono|-c|--color|--bg|--border|--fill)
            COMPREPLY=( $(compgen -W "$colors" -- "$cur") )
            return
            ;;
        --sound)
            COMPREPLY=( $(compgen -W "$sounds" -- "$cur") )
            return
            ;;
        --font)
            COMPREPLY=( $(compgen -W "$fonts" -- "$cur") )
            return
            ;;
        --fit)
            COMPREPLY=( $(compgen -W "$fits" -- "$cur") )
            return
            ;;
        --transition)
            COMPREPLY=( $(compgen -W "$transitions" -- "$cur") )
            return
            ;;
        --format)
            COMPREPLY=( $(compgen -W "$clock_formats" -- "$cur") )
            return
            ;;
        --config)
            COMPREPLY=( $(compgen -f -- "$cur") )
            return
            ;;
        -r|--resolution)
            COMPREPLY=( $(compgen -W "72x16 128x32 64x32 64x64 32x8" -- "$cur") )
            return
            ;;
        -H|--host|-p|--port|--token|--dpi|-z|--zoom|--idle-dim|--idle-brightness|--ttl|--speed|--loop|--volume|--transition-ms)
            return
            ;;
    esac

    # Subcommand-specific completions
    if [[ -n "$subcmd" ]]; then
        local common_client="-H --host -p --port --token --config --no-config"
        case "$subcmd" in
            say)
                COMPREPLY=( $(compgen -W "$common_client -c --color --bg --font --scroll --speed --transition --transition-ms --sound --ding --chime --bell --volume" -- "$cur") )
                ;;
            notify)
                COMPREPLY=( $(compgen -W "$common_client --ttl -c --color --bg --border --font --sound --ding --chime --bell" -- "$cur") )
                ;;
            gif)
                if [[ "$cur" == -* ]]; then
                    COMPREPLY=( $(compgen -W "$common_client --fill --fit --speed --loop" -- "$cur") )
                else
                    COMPREPLY=( $(compgen -f -X '!*.[gG][iI][fF]' -- "$cur") $(compgen -d -- "$cur") )
                fi
                ;;
            timer)
                if [[ "$cur" == -* ]]; then
                    COMPREPLY=( $(compgen -W "$common_client -c --color --bg --font --no-bar --sound --ding --chime --bell" -- "$cur") )
                else
                    COMPREPLY=( $(compgen -W "30s 1m 5m 10m 15m 25m 1h" -- "$cur") )
                fi
                ;;
            clock)
                COMPREPLY=( $(compgen -W "$common_client -c --color --bg --font --format" -- "$cur") )
                ;;
            alert)
                if [[ "$cur" == -* ]]; then
                    COMPREPLY=( $(compgen -W "$common_client --volume --ding --chime --bell" -- "$cur") )
                else
                    COMPREPLY=( $(compgen -W "$sounds" -- "$cur") )
                fi
                ;;
            draw)
                if [[ "$cur" == -* ]]; then
                    COMPREPLY=( $(compgen -W "$common_client --transition --transition-ms -" -- "$cur") )
                else
                    COMPREPLY=( $(compgen -f -X '!*.[jJ][sS][oO][nN]' -- "$cur") $(compgen -d -- "$cur") )
                fi
                ;;
            clear|info)
                COMPREPLY=( $(compgen -W "$common_client" -- "$cur") )
                ;;
        esac
        return
    fi

    # Top-level: offer subcommands when completing first argument, plus server flags
    if (( cword == 1 )) && [[ "$cur" != -* ]]; then
        COMPREPLY=( $(compgen -W "$subcommands" -- "$cur") )
    else
        COMPREPLY=( $(compgen -W "$server_opts $subcommands" -- "$cur") )
    fi
}

complete -F _markee markee
