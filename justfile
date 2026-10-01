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

# Configure, build and run the tests
test:
    cmake -S . -B {{build_dir}} -DCMAKE_BUILD_TYPE={{config}}
    cmake --build {{build_dir}} --config {{config}} --target zchat_tests --parallel
    ctest --test-dir {{build_dir}} --output-on-failure -C {{config}}

# Build and run zchat, passing any extra arguments (e.g. `just run -n Rex`)
[unix]
run *args: build
    ./{{build_dir}}/zchat {{args}}

# Build and run zchat, passing any extra arguments (e.g. `just run -n Rex`)
[windows]
run *args: build
    $exe = @('{{build_dir}}/{{config}}/zchat.exe', '{{build_dir}}/zchat.exe') | Where-Object { Test-Path $_ } | Select-Object -First 1; & $exe {{args}}

# Build and run zchat in the terminal instead of a window (e.g. `just run-terminal -n Rex`)
[unix]
run-terminal *args: build
    ./{{build_dir}}/zchat --terminal {{args}}

# Build and run zchat in the terminal instead of a window (e.g. `just run-terminal -n Rex`)
[windows]
run-terminal *args: build
    $exe = @('{{build_dir}}/{{config}}/zchat.exe', '{{build_dir}}/zchat.exe') | Where-Object { Test-Path $_ } | Select-Object -First 1; & $exe --terminal {{args}}

# Install zchat under PREFIX (default ~/.local, so it ends up in ~/.local/bin)
install prefix=(home_directory() / ".local"): build
    cmake --install {{build_dir}} --config {{config}} --prefix "{{prefix}}"

# Remove the build directory
clean:
    cmake -E rm -rf {{build_dir}}
