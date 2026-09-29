# Build recipes for zchat. Run `just` to list them, `just build` to build.

set windows-shell := ["powershell.exe", "-NoLogo", "-NoProfile", "-Command"]

build_dir := "build"
config := "Release"

# List available recipes
default:
    @just --list

# Configure and build zchat
build:
    cmake -S . -B {{build_dir}} -DCMAKE_BUILD_TYPE={{config}}
    cmake --build {{build_dir}} --config {{config}} --parallel

# Build and run zchat, passing any extra arguments (e.g. `just run -n Rex`)
[unix]
run *args: build
    ./{{build_dir}}/zchat {{args}}

# Build and run zchat, passing any extra arguments (e.g. `just run -n Rex`)
[windows]
run *args: build
    $exe = @('{{build_dir}}/{{config}}/zchat.exe', '{{build_dir}}/zchat.exe') | Where-Object { Test-Path $_ } | Select-Object -First 1; & $exe {{args}}

# Install zchat under PREFIX (default ~/.local, so it ends up in ~/.local/bin)
install prefix=(home_directory() / ".local"): build
    cmake --install {{build_dir}} --config {{config}} --prefix "{{prefix}}"

# Remove the build directory
clean:
    cmake -E rm -rf {{build_dir}}
