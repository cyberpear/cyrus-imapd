# 
# Copyright (c) 2000 Carnegie Mellon University.  All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer. 
#
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in
#    the documentation and/or other materials provided with the
#    distribution.
#
# 3. The name "Carnegie Mellon University" must not be used to
#    endorse or promote products derived from this software without
#    prior written permission. For permission or any other legal
#    details, please contact  
#      Office of Technology Transfer
#      Carnegie Mellon University
#      5000 Forbes Avenue
#      Pittsburgh, PA  15213-3890
#      (412) 268-4387, fax: (412) 268-7395
#      tech-transfer@andrew.cmu.edu
#
# 4. Redistributions of any form whatsoever must retain the following
#    acknowledgment:
#    "This product includes software developed by Computing Services
#     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
#
# CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
# THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
# AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
# FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
# AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
# OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
#

package Cyrus::IMAP::Admin;
use strict;
use Cyrus::IMAP;
use vars qw($VERSION
	    *create *delete *deleteacl *listacl *list *rename *setacl
	    *subscribed *quota *quotaroot);

$VERSION = '1.00';

#
# NB:  there are hooks (which error out in all cases) for IMSP support in Tcl
# cyradm.  I'll add them if I ever see what they're supposed to do... after
# coming up with perl IMSP/ACAP hooks.
#
# ASSUMPTION:  the somewhat unwieldy cyradm names are because the interpreter
# causes collisions, so I can get away with shorter versions here.
#

sub new {
  my $class = shift;
  my $self = bless {}, $class;
  $self->{cyrus} = Cyrus::IMAP->new(@_) or $self = undef;
  $self;
}

# yuck.
# I intended this to be a subclass of Cyrus::IMAP, but that's a scalar ref so
# there's nowhere to hang the error information.  Indexing a "private" hash
# with the scalar sucks fully as much IMHO.  So we forward the Cyrus::IMAP
# methods on demand.
#
# yes, this is ugly.  but the overhead is minimized this way.
sub AUTOLOAD {
  use vars qw($AUTOLOAD);
  no strict 'refs';
  $AUTOLOAD =~ s/^.*:://;
  my $sub = $Cyrus::IMAP::{$AUTOLOAD};
  *$AUTOLOAD = sub { &$sub($_[0]->{cyrus}, @_[1..$#_]); };
  goto &$AUTOLOAD;
}

sub createmailbox {
  my ($self, $mbx, $partition) = @_;
  $partition = '' if !defined($partition);
  my ($rc, $msg) = $self->send('', '', 'CREATE %s%a%a', $mbx,
			       $partition? ' ': '', $partition);
  if ($rc eq 'OK') {
    $self->{error} = undef;
    1;
  } else {
    $self->{error} = $msg;
    undef;
  }
}
*create = *createmailbox;

sub deletemailbox {
  my ($self, $mbx) = @_;
  my ($rc, $msg) = $self->send('', '', 'DELETE %s', $mbx);
  if ($rc eq 'OK') {
    $self->{error} = undef;
    1;
  } else {
    $self->{error} = $msg;
    undef;
  }
}
*delete = *deletemailbox;

sub deleteaclmailbox {
  my ($self, $mbx, @acl) = @_;
  my $cnt = 0;
  my $res = '';
  my ($rc, $msg);
  foreach my $acl (@acl) {
    ($rc, $msg) = $self->send('', '', 'DELETEACL MAILBOX %s %s', $mbx, $acl);
    if ($rc eq 'OK') {
      $cnt++;
    } else {
      $res .= "\n" if $res ne '';
      $res .= $acl . ': ' . $msg;
    }
  }
  if ($res eq '') {
    $self->{error} = undef;
  } else {
    $self->{error} = $res;
  }
  $cnt;
}
*deleteacl = *deleteaclmailbox;

sub listaclmailbox {
  my ($self, $mbx) = @_;
  my %info = ();
  $self->addcallback({-trigger => 'ACL',
		      -callback => sub {
			my %d = @_;
			return unless $d{-text} =~ s/^\"*$mbx\"*\s+//;
			while ($d{-text} =~ s/(\S+)\s+(\S+)\s*//) {
			  $d{-rock}{$1} = $2;
			}
		      },
		      -rock => \%info});
  my ($rc, $msg) = $self->send('', '', 'GETACL %s', $mbx);
  $self->addcallback({-trigger => 'ACL'});
  if ($rc eq 'OK') {
    $self->{error} = undef;
    %info;
  } else {
    $self->{error} = $msg;
    ();
  }
}
*listacl = *listaclmailbox;

sub listmailbox {
  my ($self, $pat, $ref) = @_;
  $ref ||= "";
  my @info = ();
  $self->addcallback({-trigger => 'LIST',
		      -callback => sub {
			my %d = @_;
			next unless $d{-text} =~ s/^\(([^\)]*)\) //;
			my $attrs = $1;
			my $sep = '';
			# NIL or (attrs) "sep" "str"
			if ($d{-text} =~ /^N/) {
			  return if $d{-text} !~ s/^NIL//;
			}
			elsif ($d{-text} =~ s/\"\\?(.)\"//) {
			  $sep = $1;
			}
			return unless $d{-text} =~ s/^ //;
			my $mbox;
			if ($d{-text} =~ /\"(([^\\\"]*\\)*[^\\\"]*)\"/) {
			  ($mbox = $1) =~ s/\\(.)/$1/g;
			} else {
			  $d{-text} =~ /^([]!\#-[^-~]+)/;
			  $mbox = $1;
			}
			push @{$d{-rock}}, [$mbox, $attrs, $sep];
		      },
		      -rock => \@info});
  my ($rc, $msg) = $self->send('', '', 'LIST %s %s', $ref, $pat);
  $self->addcallback({-trigger => 'LIST'});
  if ($rc eq 'OK') {
    $self->{error} = undef;
    @info;
  } else {
    $self->{error} = $msg;
    ();
  }
}
*list = \&listmailbox;

sub listsubscribed {
  my ($self, $pat, $ref) = @_;
  $ref ||= $pat;
  my @info = ();
  $self->addcallback({-trigger => 'LSUB',
		      -callback => sub {
			my %d = @_;
			next unless $d{-text} =~ s/^\(([^\)]*)\) //;
			my $attrs = $1;
			my $sep = '';
			# NIL or (attrs) "sep" "str"
			if ($d{-text} =~ /^N/) {
			  return if $d{-text} !~ s/^NIL//;
			}
			elsif ($d{-text} =~ s/\"\\?(.)\"//) {
			  $sep = $1;
			}
			return unless $d{-text} =~ s/^ //;
			my $mbox;
			if ($d{-text} =~ /\"(([^\\\"]*\\.)*[^\\\"]*)\"/) {
			  ($mbox = $1) =~ s/\\(.)/$1/g;
			} else {
			  $d{-text} =~ /^([]!\#-[^-~]+)/;
			  $mbox = $1;
			}
			push @{$d{-rock}}, [$mbox, $attrs, $sep];
		      },
		      -rock => \@info});
  my ($rc, $msg) = $self->send('', '', 'LSUB %s %s', $pat, $ref);
  $self->addcallback({-trigger => 'LSUB'});
  if ($rc eq 'OK') {
    $self->{error} = undef;
    @info;
  } else {
    $self->{error} = $msg;
    ();
  }
}
*subscribed = \&listsubscribed;

sub listquota {
  my ($self, $root) = @_;
  my @info = ();
  $self->addcallback({-trigger => 'QUOTA',
		      -callback => sub {
			my %d = @_;
			next unless
			  $d{-text} =~ s/^\S+ \((\S+) (\S+) (\S+)\)//;
			push @{$d{-rock}}, $1, [$2, $3];
		      },
		      -rock => \@info});
  my ($rc, $msg) = $self->send('', '', 'GETQUOTA %s', $root);
  $self->addcallback({-trigger => 'QUOTA'});
  if ($rc eq 'OK') {
    $self->{error} = undef;
    @info;
  } else {
    $self->{error} = $msg;
    ();
  }
}
*quota = *listquota;

sub listquotaroot {
  my ($self, $root) = @_;
  my $qr = '';
  my @info = ();
  $self->addcallback({-trigger => 'QUOTAROOT',
		      -callback => sub {
			my %d = @_;
			return unless $d{-text} =~ /^\S+ (\S+)/;
			${$d{-rock}} = $1;
		      },
		      -rock => \$qr},
		     {-trigger => 'QUOTA',
		      -callback => sub {
			my %d = @_;
			return unless
			  $d{-text} =~ s/^\S+ \((\S+) (\S+) (\S+)\)//;
			push @{$d{-rock}}, $1, [$2, $3];
		      },
		      -rock => \@info});
  my ($rc, $msg) = $self->send('', '', 'GETQUOTAROOT %s', $root);
  $self->addcallback({-trigger => 'QUOTA'}, {-trigger => 'QUOTAROOT'});
  if ($rc eq 'OK') {
    $self->{error} = undef;
    ($qr, @info);
  } else {
    $self->{error} = $msg;
    ();
  }
}
*quotaroot = *listquotaroot;

sub renamemailbox {
  my ($self, $src, $dest, $ptn) = @_;
  my ($rc, $msg) = $self->send('', '', 'RENAME %s %s%a%a', $src, $dest,
			       $ptn ? ' ' : $ptn, $ptn);
  if ($rc eq 'OK') {
    $self->{error} = undef;
    1;
  } else {
    $self->{error} = $msg;
    undef;
  }
}
*rename = *renamemailbox;

# hm.  this list can't be confused with valid ACL values as of 1.6.19, except
# for "all".  sigh.
my %aclalias = (none => '',
		read => 'lrs',
		post => 'lrsp',
		append => 'lrsip',
		write => 'lrswipcd',
		all => 'lrswipcda');

sub setaclmailbox {
  my ($self, $mbx, %acl) = @_;
  my $cnt = 0;
  my $res = '';
  my ($rc, $msg);
  foreach my $id (keys %acl) {
    $acl{$id} = $aclalias{$acl{$id}} if defined $aclalias{$acl{$id}};
    ($rc, $msg) = $self->send('', '', 'SETACL %s %s %s', $mbx, $id, $acl{$id});
    if ($rc eq 'OK') {
      $cnt++;
    } else {
      $res .= "\n" if $res ne '';
      $res .= $id . ': ' . $acl{$id} . ': ' . $msg;
    }
  }
  if ($rc eq 'OK') {
    $self->{error} = undef;
    $cnt;
  } else {
    $self->{error} = $res;
    undef;
  }
}
*setacl = *setaclmailbox;

sub setquota {
  my ($self, $mbx, %quota) = @_;
  foreach my $id (keys %quota) {
    if ($id !~ /^[]!\#-[^-~]+$/) {
	$self->{error} = $id . ': not an atom';
	return undef;
    }
    if ($quota{$id} !~ /^\d+$/) {
	$self->{error} = $id . ': ' . $quota{$id} . ': not a number';
	return undef;
    }
  }
  my ($rc, $msg) = $self->send('', '', 'SETQUOTA %s (%v)', $mbx, \%quota);
  if ($rc eq 'OK') {
    $self->{error} = undef;
    1;
  } else {
    $self->{error} = $msg;
    undef;
  }
}

sub error {
  my $self = shift;
  $self->{error};
}

1;
__END__

=head1 NAME

Cyrus::IMAP::Admin - Cyrus administrative interface Perl module

=head1 SYNOPSIS

  use Cyrus::IMAP::Admin;

  my $client = Cyrus::IMAP::Admin->new('mailhost'[, $flags]);
  $rc = $client->create('user.auser'[, $partition]);
  $rc = $client->delete('user.auser');
  $rc = $client->deleteacl('user.buser', 'user1', 'user2');
  %acls = $client->listacl('user.buser');
  @mailboxes = $client->list('*');
  @mailboxes = $client->list('%', 'user.');
  @mailboxes = $client->subscribed('*');
  %quota = $client->quota($root);
  ($root, %quota) = $client->quotaroot($mailbox);
  $rc = $client->rename($old, $new[, $partition]);
  $rc = $client->setacl($mailbox, $user =E<gt> $acl[, ...]);
  $rc = $client->setquota($mailbox, $resource =E<gt> $quota[, ...]);

=head1 DESCRIPTION

This module is a Perl interface to Cyrus administrative functions.  It is used
to implement Cyrus::IMAP::Admin::Shell (otherwise known as B<cyradm> and also
available for use in Perl administrative programs.

=head1 METHODS

Many of the methods have a B<cyradm>-compatible name and a shorter name.
The shorter name is shown in the synopsis when it exists; the
B<cyradm>-compatible name should be reasonably obvious.

In general, methods return undef or empty lists on error.  In some cases
a method may return an empty list without an error (i.e. C<list> of a
nonexistent hierarchy), so it may be necessary to check the error state
explicitly via the C<error> method.

=over 4

=item new($server[, $flags])

Instantiates a B<cyradm> object.  This is in fact an Cyrus::IMAP object with
a few additional methods, so all Cyrus::IMAP methods are available if needed.
(In particular, you will always want to use the C<authenticate> method.)

=item error

Return the last error that occurred, or undef if the last operation was
successful.  This is in some cases (such as C<list>) the only way to
distinguish between a successful return of an empty list and an error return.

Calling C<error> does not reset the error state, so it is legal to write:

    @folders = $cyradm->list($spec);
    print STDERR "Error: ", $cyradm->error if $cyradm->error;

=item createmailbox($mailbox[, $partition])

=item create($mailbox[, $partition])

Create a new mailbox on the specified or default partition.

=item deletemailbox($mailbox)

=item delete($mailbox)

Delete a mailbox.  As with B<cyradm>, you will probably need to add the B<d>
ACL for yourself to the mailbox with C<setaclmailbox> first.

=item deleteaclmailbox($mailbox, $user[, ...])

=item deleteacl($mailbox, $user[, ...])

Delete one or more ACL from a mailbox.

=item listaclmailbox($mailbox)

=item listacl($mailbox)

Returns a hash of mailbox ACLs, with each key being a Cyrus user and the
corresponding value being the ACL.

=item listmailbox($pattern[, $reference])

=item list($pattern[, $reference])

List mailboxes matching the specified pattern, starting from the specified
reference.  The result is a list; each element is an array containing the
mailbox name, attributes, and the separator.  (This interface may change.)

=item listsubscribed($pattern[, $reference])

=item subscribed($pattern[, $reference])

Like C<listmailbox> but only shows subscribed mailboxes.

=item listquota($root)

=item quota($root)

Returns a hash specifying the quota for the specified quota root.  Use
C<listquotaroot> to find the quota root for a mailbox.

=item listquotaroot($mailbox)

=item quotaroot($mailbox)

Returns a list, the first element is the quota root for the mailbox and
remaining elements are a hash specifying its quota.

=item renamemailbox($from, $to[, $partition])

=item rename($from, $to[, $partition])

Renames the specified mailbox, optionally moving it to a different partition.

=item setaclmailbox($mailbox, $user =E<gt> $acl[, ...])

=item setacl($mailbox, $user =E<gt> $acl[, ...])

Set ACLs on a mailbox.  The ACL may be one of the special strings C<none>,
C<read> (C<lrs>), C<post> (C<lrsp>), C<append> (C<lrsip>), C<write>
(C<lrswipcd>), or C<all> (C<lrswipcda>), or any combinations of the ACL codes:

=over 4

=item l

Lookup (visible to LIST/LSUB/UNSEEN)

=item r

Read (SELECT, CHECK, FETCH, PARTIAL, SEARCH, COPY source)

=item s

Seen (STORE \SEEN)

=item w

Write flags other than \SEEN and \DELETED

=item i

Insert (APPEND, COPY destination)

=item p

Post (send mail to mailbox)

=item c

Create (subfolders)

=item d

Delete (STORE \DELETED, EXPUNGE)

=item a

Administer (SETACL)

=back

=item setquota($mailbox, $resource, $quota[, ...])

Set quotas on a mailbox.  Note that Cyrus currently only defines one resource,
C<STORAGE>.

=back

=head1 AUTHOR

Brandon S. Allbery, allbery@ece.cmu.edu

=head1 SEE ALSO

Cyrus::IMAP
Cyrus::IMAP::Shell
perl(1), cyradm(1), imapd(8).

=cut
