FROM fedora:rawhide

RUN dnf update -y \
    && dnf install -y 'dnf-command(builddep)' \
    && dnf builddep -y glib-networking \
    && dnf install -y clang-analyzer \
                      crypto-policies-scripts \
                      libasan \
                      openssl-devel \
    && dnf clean all

# Build our own glib
RUN dnf install -y git \
    && git clone --depth=1 https://gitlab.gnome.org/GNOME/glib.git \
    && mkdir -p glib/_build \
    && cd glib/_build \
    && meson .. --prefix=/usr \
    && meson compile \
    && meson install

ARG HOST_USER_ID=5555
ENV HOST_USER_ID ${HOST_USER_ID}
RUN useradd -u $HOST_USER_ID -ms /bin/bash user
RUN update-crypto-policies --set DEFAULT:FEDORA32

USER user
WORKDIR /home/user

ENV LANG C.UTF-8
