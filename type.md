# Types
Primitive and composite types and type system.

## Intent
A wide variety of primitives are supported to help with types and simple, clean and meaningful coding. While many types are just an alias to other type, they provide clear meaning.

## Primitive Types
```cmd
int     // 32 bit
int8
int16
int32
int64

uint    // 32 bit
uint8
uint16
uint32
uint64

float    // 32 bit
float16
float32
float64

bool    // 8 bit

char    // 8 bit
rune    // 32 bit

string  // ascii string
strand  // utf8 string

byte

// ... Reserved ...

complex     // 64 bit
complex64
complex128
```

## Scheme
#### 1. Handling many types:
As there's many (too many?) types, a way to reduce footprint is necessary. Type aliasing can be used. For inspiration there's **typedef** in c/c++ **type** in go.
