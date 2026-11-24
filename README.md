# sg

A simple command-line utility for searching text using multiple search terms.

> [!WARNING]
> This project is a work in progress. Development happens whenever time and motivation allow.

This project is developed on M1 architecture and built with clang.

## Build

Bootstrap the build system.

```sh
$ cc -o nob nob.c
```

Build `sg`.

```sh
$ ./nob
```

## Tutorial

Search for every line that contains A.

```sh
$ sg A
```

Search for every line that contains A and B.

```sh
$ sg A B
```

To word match on a term prefix with `w:`.

```sh
$ sg w:A
```

## Credits

- [nob.h](https://github.com/tsoding/nob.h)

## License

This project is licensed under the [MIT License](LICENSE).
Third-party licenses can be found in [LICENSES/](LICENSES/).
