# Generated by the protocol buffer compiler.  DO NOT EDIT!
# source: model.proto

import sys
_b=sys.version_info[0]<3 and (lambda x:x) or (lambda x:x.encode('latin1'))
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from google.protobuf import reflection as _reflection
from google.protobuf import symbol_database as _symbol_database
from google.protobuf import descriptor_pb2
# @@protoc_insertion_point(imports)

_sym_db = _symbol_database.Default()




DESCRIPTOR = _descriptor.FileDescriptor(
  name='model.proto',
  package='',
  syntax='proto3',
  serialized_pb=_b('\n\x0bmodel.proto\"\x18\n\x04\x64\x61ta\x12\x10\n\x08\x65nc_data\x18\x01 \x01(\x0c\x62\x06proto3')
)
_sym_db.RegisterFileDescriptor(DESCRIPTOR)




_DATA = _descriptor.Descriptor(
  name='data',
  full_name='data',
  filename=None,
  file=DESCRIPTOR,
  containing_type=None,
  fields=[
    _descriptor.FieldDescriptor(
      name='enc_data', full_name='data.enc_data', index=0,
      number=1, type=12, cpp_type=9, label=1,
      has_default_value=False, default_value=_b(""),
      message_type=None, enum_type=None, containing_type=None,
      is_extension=False, extension_scope=None,
      options=None),
  ],
  extensions=[
  ],
  nested_types=[],
  enum_types=[
  ],
  options=None,
  is_extendable=False,
  syntax='proto3',
  extension_ranges=[],
  oneofs=[
  ],
  serialized_start=15,
  serialized_end=39,
)

DESCRIPTOR.message_types_by_name['data'] = _DATA

data = _reflection.GeneratedProtocolMessageType('data', (_message.Message,), dict(
  DESCRIPTOR = _DATA,
  __module__ = 'model_pb2'
  # @@protoc_insertion_point(class_scope:data)
  ))
_sym_db.RegisterMessage(data)


# @@protoc_insertion_point(module_scope)