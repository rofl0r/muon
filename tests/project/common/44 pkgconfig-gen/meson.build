project(
    'pkgconfig-gen',
    'c',
    meson_version: '>=0.60.0',
    default_options: ['default_library=shared'],
)

# Some CI runners does not have zlib, just skip them as we need some common
# external dependency.
cc = meson.get_compiler('c')
if not cc.find_library('z', required: false).found()
    error('MESON_SKIP_TEST: zlib missing')
endif

# First check we have pkg-config >= 0.29
pkgconfig = find_program('pkg-config', required: false)
if not pkgconfig.found()
    error('MESON_SKIP_TEST: pkg-config not found')
endif

v = run_command(pkgconfig, '--version', check: true).stdout().strip()
if v.version_compare('<0.29')
    error('MESON_SKIP_TEST: pkg-config version \'' + v + '\' too old')
endif

python = find_program('python3')
fs = import('fs')
pkgg = import('pkgconfig')

lib = shared_library('simple', 'simple.c')
libver = '1.0'
install_headers('simple.h')

to_validate = []

# generate for systems without a glib pc file
pkgg.generate(name: 'glib-2.0', dataonly: true)

pkgg.generate(
    libraries: [lib, '-lz'],
    subdirs: '.',
    version: libver,
    name: 'libsimple',
    filebase: 'simple',
    description: 'A simple demo library.',
    requires: 'glib-2.0', # Not really, but only here to test that this works.
    requires_private: ['gio-2.0', 'gobject-2.0'],
    libraries_private: [lib, '-lz'],
)
to_validate += 'simple'

answerlib = shared_library('answer', 'answer.c')

pkgg.generate(
    answerlib,
    name: 'libanswer',
    description: 'An answer library.',
    extra_cflags: ['-DLIBFOO'],
)
to_validate += 'libanswer'

# Test that name_prefix='' and name='libfoo' results in '-lfoo'
lib2 = shared_library(
    'libfoo',
    'foo.c',
    link_with: answerlib,
    name_prefix: '',
    version: libver,
)

pkgg.generate(
    lib2,
    libraries: [lib2, answerlib],
    name: 'libfoo',
    version: libver,
    description: 'A foo library.',
    variables: ['foo=bar', 'datadir=${prefix}/data'],
    extra_cflags: ['-DLIBFOO'],
)
to_validate += 'libfoo'

pkgg.generate(
    name: 'libhello',
    description: 'A minimalistic pkgconfig file.',
    version: libver,
)
to_validate += 'libhello'

pkgg.generate(
    name: 'libhello_nolib',
    description: 'A minimalistic pkgconfig file.',
    version: libver,
    dataonly: true,
    variables: {
        'foo': 'bar',
        # prefix is not set by default for dataonly pc files, but it is allowed to
        # define it manually.
        'prefix': get_option('prefix'),
        'escaped_var': 'hello world',
    },
    unescaped_variables: {
        'unescaped_var': 'hello world',
    },
)
to_validate += 'libhello_nolib'

# Regression test for 2 cases:
# - link_whole from InternalDependency used to be ignored, but we should still
#   recurse to add libraries they link to. In this case it must add `-lsimple1`
#   in generated pc file.
# - dependencies from InternalDependency used to be ignored. In this it must add
#   `-lz` in generated pc file.
simple1 = shared_library('simple1', 'simple.c')
stat1 = static_library('stat1', 'simple.c', link_with: simple1)
dep = declare_dependency(link_whole: stat1, dependencies: cc.find_library('z'))
simple2 = library('simple2', 'simple.c')
pkgg.generate(simple2, libraries: dep)
to_validate += 'simple2'

# Regression test: as_system() does a deepcopy() of the InternalDependency object
# which caused `-lsimple3` to be duplicated because generator used to compare
# Target instances instead of their id.
simple3 = shared_library('simple3', 'simple.c')
dep1 = declare_dependency(link_with: simple3)
dep2 = dep1.as_system()
pkgg.generate(libraries: [dep1, dep2], name: 'simple3', description: 'desc')
to_validate += 'simple3'

# Regression test: stat2 is both link_with and link_whole, it should not appear
# in generated pc file.
stat2 = static_library('stat2', 'simple.c', install: true)
simple4 = library('simple4', 'simple.c', link_with: stat2)
simple5 = library('simple5', 'simple5.c', link_with: simple4, link_whole: stat2)
pkgg.generate(simple5)
to_validate += 'simple5'

# Test passing a linkable CustomTarget and CustomTargetIndex to generator.
# Do this only with gcc/clang to not have to deal with other compiler command
# line specificities.
if cc.get_id() in ['gcc', 'clang']
    ct = custom_target(
        'ct',
        input: 'simple.c',
        output: 'libct.so',
        command: [cc.cmd_array(), '@INPUT@', '-shared', '-o', '@OUTPUT@'],
    )
    pkgg.generate(libraries: ct, name: 'ct', description: 'custom target')
    to_validate += 'ct'

    pkgg.generate(
        libraries: ct[0],
        name: 'ct0',
        description: 'custom target index',
    )
    to_validate += 'ct0'
endif

# Regression test: A library linking to an uninstalled custom_target static
# library used to crash when generating its pkgconfig file.
# Copy libstat2.a to libstat3.a to have a static library as custom target.
infile = stat2.full_path()
outfile = meson.current_build_dir() / 'libstat3.a'
script = 'import shutil ; shutil.copyfile("@0@", "@1@")'.format(infile, outfile)
ct = custom_target(
    'stat3',
    input: stat2,
    output: fs.name(outfile),
    command: [python, '-c', script],
)
simple6 = library('simple6', 'dependencies/dummy.c', link_with: ct)
pkgg.generate(simple6)
to_validate += 'simple6'

# implicit variables
pkgg.generate(
    name: 'libvartest',
    description: 'Check that implicit vars are created',
    version: libver,
    variables: [
        'datadir=${prefix}/data',
        'foo=${datadir}/foo',
        'bar=${bindir}/bar',
    ],
)
to_validate += 'libvartest'

pkgg.generate(
    name: 'libvartest2',
    description: 'Check that libdir is not an implicit var',
    version: libver,
    variables: ['bar=${libdir}/bar'],
)
to_validate += 'libvartest2'

cmp = find_program('diff')


privder = 'muon-private'
env = environment()
env.prepend('PKG_CONFIG_PATH', meson.current_build_dir() / privder)

foreach pc : to_validate
    test(
        'validate' + pc,
        pkgconfig,
        args: ['--validate', pc],
        env: env,
    )

    test(
        'diff ' + pc,
        cmp,
        args: [
            files('expected' / pc + '.pc'),
            meson.current_build_dir() / privder / pc + '.pc',
        ],
    )
endforeach
# Regression test: variables kwarg should listify single string value
pkgg.generate(
    name: 'libvartest3',
    description: 'Check that variables can be single string',
    variables: 'foo=bar',
)
