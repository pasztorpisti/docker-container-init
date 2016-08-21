
# docker-container-init

Just another simple `init` to reap zombies inside your docker containers.
It can also be useful when you want to use containers as lightweight "VM"s.

## Problems to solve

### Zombie attack inside the container

The entrypoint process (pid 1) of your container might not collect
inherited zombie processes. This isn't a problem if the processes inside
the container clean up their own child processes correctly.

However, other processes like grand-grand children or those that appear
as a result of `docker exec`ing into the container might leave orphaned
processes and zombies in your container. These orphaned processes are
reparented to the main (pid 1) process of your container.

Besides reaping zombies a custom `init` can handle incoming signals and
container shutdown in a better way than a quick n' dirty shell script.

### Using a container as a "VM"

A custom `init` might come handy as the main process of a container that
you want to use as a "VM" (for example as a reproducible work environment).

In case of using a container as "VM" you probably just need something that
keeps your "VM" alive and then you `docker exec` into the container to
login with shells. The process that keeps your VM alive can be this
dumb `init` - it can shut down your services gracefully as you will see
in examples below.

## Usage

### Compiling `docker-container-init.c`

In order to use docker-container-init, first you have to compile it.
The `build.sh` script will do that for you by performing compilation
inside a docker container using the distro of your choice and placing
the binary outside of the container. It can be parametrized using
environment variables. Examples:

Builds a dynamically linked `docker-container-init` for the `debian:latest` image:
```sh
./build.sh
```

Static linking. A statically linked binary is larger than a dynamically linked
one (~1MB instead of a few kilobytes) but it doesn't have dependencies and
you have better chances to be able to use it with many different distros
and distro versions.
```sh
STATIC_LINK=1 ./build.sh
```

Builds a dynamically linked `docker-container-init` for the `centos:latest` image:
```sh
IMAGE=centos ./build.sh
```

Building on `ubuntu:16.04` image:
```sh
IMAGE=ubuntu:16.04 ./build.sh
```

Placing the compiled binary to a custom location. The target directory must exists.
```sh
OUTPUT_FILENAME=~/my_custom_dir/my_init_filename ./build.sh
```

### Using `docker-container-init` in your docker images/containers

#### Reaping zombies in your existing containers

Let's assume you've already compiled the `docker-container-init` binary for
the platform you're using inside your container and the original `Dockerfile`
of your container looks like this:

```
FROM debian
ADD . /scripts
ENTRYPOINT /scripts/entrypoint.sh arg0 arg1
```

Now let's copy the `docker-container-init` binary next to the `Dockerfile`
and change `Dockerfile` to this:

```
FROM debian
ADD . /scripts
ENTRYPOINT ["/scripts/docker-container-init", "/scripts/entrypoint.sh", "arg0", "arg1"]
```

Warning!!! When you specify your entrypoint it is very important to use a
json array as the parameter of the `ENTRYPOINT` statement!!!
If you don't use the json array syntax then our `docker-container-init` is
executed as a subcommand of a shell. This is bad because the shell will be
pid 1 while our `docker-container-init` will have a different pid. If you
make the mistake of not using a json array then `docker-container-init`
terminates with the following error message:

```
[ERROR][docker-container-init] You have to either run this as pid 1 or specify the -D option
```

#### When you use containers as "VM"s

You could actually use a simple shell script or a `/bin/bash` as the init
of your container-as-VM however that wouldn't be able to handle zombies
and orphaned children easily or that well.
Our `docker-container-init` handles zombies and it's shutdown mechanism
allows you to run and terminate some background services easily.

In case of a container you probably don't want `docker-container-init` to
launch anything for your as the "main" process of your container, you need
only a few services. But where do you start up your services? If you write
a script and you execute it with `docker-container-init` then after
starting up the services from the script those services simply become
daemonised, orphaned, and adopted by our `init(1)` process and your script
exits causing `docker-container-init` to exit (since it terminates when its
main command exists). To aid this you probably want an infinite sleep loop in
your script just to prevent `docker-container-init` from exiting... This
isn't really a good solution.

The good solution to this problem is launching a shell script as the
entrypoint of your container. From your shell script you start up the
services in the background and then as a last step of your script your
perform an `exec docker-container-init` that replaces the shell process
(that is actually pid 1) with the `docker-container-init` program. This way
you can start up your services that are adopted by pid 1 but you can still
run `docker-container-init` without asking it to launch a "main" program
for you. This way `docker-container-init` will terminate only if you send it
a `SIGTERM` or `SIGINT` and before actually exiting it politely asks all
adopted children to finish. More about this graceful shutdown mechanism
later...

Example: Let's assume that you have a directory that contains only a
`Dockerfile`, `entrypoint.sh` and the `docker-container-init` binary.

Dockerfile:
```
FROM debian
ADD . /scripts
ENTRYPOINT ["/scripts/entrypoint.sh"]
```

Again: it is important to use a json array as the parameter
of ENTRYPOINT to launch our process directly as pid 1.

entrypoint.sh:
```sh
#!/bin/bash
set -euo pipefail

# The directory that holds this script becomes our new working directory:
cd "$( dirname "${BASH_SOURCE[0]}" )"

# Launching services...
service whatever_service start
./your_service_script.sh &

# As a last step we replace this shell process with docker-container-init:
exec ./docker-container-init
```

Now let's build this "VM", start it up, and then login:

```sh
# building the docker image
docker build -t vm_test .

# creating and starting a container
docker run -dit -h vm_test --name vm_test vm_test

# "logging in"
docker exec -it vm_test /bin/bash

# gracefully stopping the "VM"
docker stop vm_test
```

## The lifecycle of a `docker-container-init` process

The lifecycle of `docker-container-init` can be separated into 3 stages:

1. Starting up
2. Running
3. Shutdown sequence

### 1. Starting up

The `docker-container-init` program accepts a few optional flags and
an optional command to be launched as the "main process" of the container:

```
woof@jessie:~$ ./docker-container-init -h
docker-container-init built on Aug 21 2016 12:40:02

Usage: ./docker-container-init [options] [--] [command]

Options:
-W  Don't wait for all children (including inherited/orphaned ones) before
    exit. This wait is performed after your command (if any) has exited.
-B  Don't broadcast a sigterm before waiting for all children.
    This option is ignored when -W is used.
-I  Don't exit on SIGINT. Exit only on SIGTERM.
    This option is ignored when you specify a command.
-g  Run your command in its own process group and forward SIGTERM to the
    group instead of the process created from your command.
-r  Enable forwarding of realtime signals to the specified command.
    Without this option we forward only some of the standard signals.
-D  Don't check whether this process is running as pid 1.
    Comes in handy for debugging.
-v  Log a limited number of info messages to stderr.
    Without -v we log only in case of errors.
-vv Spammy debug log level.
-h  Print this help message.
```

You can start up `docker-container-init` in two different ways:

1. If you specify a command then `docker-container-init` creates a
   subprocesses from it and starts its shutdown sequence only when the
   executed command terminates. The exit status of `docker-container-init`
   will be the same as that of the executed command.
2. Without a command parameter `docker-container-init` simply starts up and
   it is waiting for a `SIGINT` or `SIGTERM` to start the shutdown sequence.
   Note that in this case you can disable `SIGINT` as a shutdown sequence
   initiator by using the `-I` flag.

### 2. Running

While running `docker-container-init` as pid 1 it adopts orphaned
children and reaps zombie processes.

If you've launched `docker-container-init` by specifying a command parameter
then signals received by `docker-container-init` are forwarded to the
running subcommand. This includes `SIGHUP`, `SIGINT`, `SIGTERM`, `SIGQUIT`,
`SIGUSR1`, `SIGUSR2` and realtime signals too if you've used the `-r` flag.
If you've specified the `-g` flag then the subcommand runs in its own
process group and unlike other signals, `SIGTERM` is frowarded to the process
group instead of the leading process created from your command.

### 3. Shutdown sequence

If you've specified a command parameter for `docker-container-init` then the
shutdown sequence starts when the command exits. Otherwise the shutdown
sequence starts when `docker-container-init` receives `SIGINT` or `SIGTERM`.

When the shutdown sequence starts it might happen that there are still some
services running (probably as a child process of pid 1 that is
`docker-container-init`). There can also be some orphaned-adopted subcommands
that need a few seconds to finish. To deal with these situations the
shutdown sequence does two things by default:

1. Broadcasts `SIGTERM` to every process
2. Waits for all child processes to terminate

If this default shutdown machanism isn't a good fit then you can customize it:
The `-B` flag turns off step #1. The `-W` flag turns off both #1 and #2.

In general a shutdown mechanism usually involves sending a `SIGTERM` and
after a given timeout sending a `SIGKILL`. In contrast `docker-container-init`
doesn't use any timeouts and never sends `SIGKILL` to any subprocesses.
The reason for this is that a container is usually stopped from outside
either by the `docker stop` command or the shutdown sequence of the host
machine that terminates the docker service. Both of these implement the
timeout + `SIGKILL` combo, there is no need to complicate it further.

