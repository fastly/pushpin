TEMPLATE = subdirs

src.subdir = src

postbuild.subdir = postbuild
postbuild.depends = src

SUBDIRS += src postbuild
