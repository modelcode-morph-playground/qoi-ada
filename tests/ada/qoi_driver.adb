--  Ada half of the differential test of the C++ port against the original Ada
--  package QOI (see tests/ada_diff_test.cpp for the file protocol).
--
--  Usage: qoi_driver <directory>
--
--  Reads <directory>/manifest.txt. For every line
--
--     E <id> <width> <height> <channels> <colorspace>
--        QOI.Encode of <id>.raw, result written to <id>.ada.qoi (an empty
--        file if Encode reports failure);
--     D <id> [<raw-id>]
--        QOI.Decode of <id>.in, status and header written to <id>.ada.res,
--        pixels to <id>.ada.dec.
--
--  Decoding follows the rules shared with the C++ side:
--    * data shorter than 22 bytes is a reject without calling Decode (the
--      Ada precondition requires 22 bytes);
--    * an invalid header (Get_Desc empty, zero dimension, channels not 3 or 4)
--      is passed to Decode with a 4-byte buffer, which must report failure;
--    * an image of more than 2**24 bytes is a "skip".
--
--  The exit status is 1 if any case raised an exception.

with Ada.Command_Line;
with Ada.Exceptions;
with Ada.Streams;               use Ada.Streams;
with Ada.Streams.Stream_IO;
with Ada.Strings;
with Ada.Strings.Fixed;
with Ada.Text_IO;
with Ada.Unchecked_Deallocation;
with System.Storage_Elements;   use System.Storage_Elements;

with QOI;

procedure QOI_Driver is

   package SIO renames Ada.Streams.Stream_IO;
   package TIO renames Ada.Text_IO;

   Max_Pixel_Bytes : constant Storage_Count := 2 ** 24;

   type Storage_Array_Access is access Storage_Array;
   procedure Free is new Ada.Unchecked_Deallocation
     (Storage_Array, Storage_Array_Access);

   Failures : Natural := 0;
   Done     : Natural := 0;

   -----------
   -- Image --
   -----------

   function Image (X : Long_Long_Integer) return String is
     (Ada.Strings.Fixed.Trim (Long_Long_Integer'Image (X), Ada.Strings.Left));

   function Image (X : Storage_Count) return String is
     (Image (Long_Long_Integer (X)));

   ---------------
   -- Read_File --
   ---------------

   function Read_File (Name : String) return Storage_Array is
      File : SIO.File_Type;
   begin
      SIO.Open (File, SIO.In_File, Name);
      declare
         Len    : constant Storage_Count := Storage_Count (SIO.Size (File));
         Buf    : Stream_Element_Array (1 .. Stream_Element_Offset (Len));
         Last   : Stream_Element_Offset;
         Result : Storage_Array (1 .. Len);
      begin
         if Len > 0 then
            SIO.Read (File, Buf, Last);
         end if;
         SIO.Close (File);
         for I in 1 .. Len loop
            Result (I) := Storage_Element (Buf (Stream_Element_Offset (I)));
         end loop;
         return Result;
      end;
   end Read_File;

   ----------------
   -- Write_File --
   ----------------

   procedure Write_File
     (Name  : String;
      Data  : Storage_Array;
      Count : Storage_Count)
   is
      File : SIO.File_Type;
   begin
      SIO.Create (File, SIO.Out_File, Name);
      if Count > 0 then
         declare
            Buf : Stream_Element_Array (1 .. Stream_Element_Offset (Count));
         begin
            for I in 1 .. Count loop
               Buf (Stream_Element_Offset (I)) :=
                 Stream_Element (Data (Data'First + I - 1));
            end loop;
            SIO.Write (File, Buf);
         end;
      end if;
      SIO.Close (File);
   end Write_File;

   ----------------
   -- Write_Text --
   ----------------

   procedure Write_Text (Name : String; Text : String) is
      File : TIO.File_Type;
   begin
      TIO.Create (File, TIO.Out_File, Name);
      TIO.Put (File, Text);
      TIO.Close (File);
   end Write_Text;

   function Desc_Line (Desc : QOI.QOI_Desc) return String is
     (Image (Desc.Width) & " " & Image (Desc.Height) & " "
      & Image (Desc.Channels) & " "
      & (if Desc.Colorspace = QOI.SRGB then "0" else "1"));

   -----------------
   -- Encode_Case --
   -----------------

   procedure Encode_Case
     (Dir        : String;
      Id         : String;
      W, H, C    : Storage_Count;
      Colorspace : Natural)
   is
      Desc     : constant QOI.QOI_Desc :=
        (Width      => W,
         Height     => H,
         Channels   => C,
         Colorspace =>
           (if Colorspace = 0 then QOI.SRGB else QOI.SRGB_Linear_Alpha));
      Pix      : constant Storage_Array := Read_File (Dir & "/" & Id & ".raw");
      Out_Name : constant String := Dir & "/" & Id & ".ada.qoi";
   begin
      if not QOI.Valid_Size (Desc) or else Pix'Length /= W * H * C then
         Write_File (Out_Name, Pix, 0);
         return;
      end if;

      declare
         Worst  : constant Storage_Count := QOI.Encode_Worst_Case (Desc);
         Output : Storage_Array (1 .. Worst);
         Size   : Storage_Count;
      begin
         QOI.Encode (Pix, Desc, Output, Size);
         Write_File (Out_Name, Output, Size);
      end;
   end Encode_Case;

   -----------------
   -- Decode_Case --
   -----------------

   procedure Decode_Case (Dir : String; Id : String) is
      Data     : constant Storage_Array := Read_File (Dir & "/" & Id & ".in");
      Res_Name : constant String := Dir & "/" & Id & ".ada.res";
      Dec_Name : constant String := Dir & "/" & Id & ".ada.dec";
      Desc     : QOI.QOI_Desc;
      Desc2    : QOI.QOI_Desc;
   begin
      if Data'Length < QOI.QOI_HEADER_SIZE + QOI.QOI_PADDING'Length then
         Write_Text (Res_Name, "reject" & ASCII.LF & "0 0 0 0" & ASCII.LF);
         return;
      end if;

      QOI.Get_Desc (Data, Desc);

      if Desc.Width = 0 or else Desc.Height = 0
        or else Desc.Channels not in 3 .. 4
      then
         declare
            Tiny : Storage_Array (1 .. 4);
            Size : Storage_Count;
         begin
            QOI.Decode (Data, Desc2, Tiny, Size);
            Write_Text
              (Res_Name,
               (if Size = 0 then "reject" else "accept") & ASCII.LF
               & Desc_Line (Desc) & ASCII.LF);
         end;
         return;
      end if;

      if Desc.Width > Max_Pixel_Bytes / Desc.Height / Desc.Channels then
         Write_Text
           (Res_Name, "skip" & ASCII.LF & Desc_Line (Desc) & ASCII.LF);
         return;
      end if;

      declare
         Needed : constant Storage_Count :=
           Desc.Width * Desc.Height * Desc.Channels;
         Output : Storage_Array_Access := new Storage_Array (1 .. Needed);
         Size   : Storage_Count;
      begin
         QOI.Decode (Data, Desc2, Output.all, Size);
         if Size = 0 then
            Write_Text
              (Res_Name, "reject" & ASCII.LF & Desc_Line (Desc) & ASCII.LF);
         else
            Write_File (Dec_Name, Output.all, Size);
            Write_Text
              (Res_Name,
               "accept" & ASCII.LF & Desc_Line (Desc2) & ASCII.LF);
         end if;
         Free (Output);
      exception
         when others =>
            Free (Output);
            raise;
      end;
   end Decode_Case;

   ------------------
   -- Is_Separator --
   ------------------

   function Is_Separator (C : Character) return Boolean is
     (C = ' ' or else C = ASCII.CR or else C = ASCII.HT);

   type Token is record
      First, Last : Natural;
   end record;
   type Token_Array is array (1 .. 8) of Token;

   -----------
   -- Split --
   -----------

   procedure Split
     (Line : String; Tokens : out Token_Array; Count : out Natural)
   is
      I : Natural := Line'First;
      J : Natural;
   begin
      Tokens := (others => (First => 1, Last => 0));
      Count := 0;
      while I <= Line'Last loop
         if Is_Separator (Line (I)) then
            I := I + 1;
         else
            J := I;
            while J < Line'Last and then not Is_Separator (Line (J + 1)) loop
               J := J + 1;
            end loop;
            if Count < Tokens'Last then
               Count := Count + 1;
               Tokens (Count) := (First => I, Last => J);
            end if;
            I := J + 1;
         end if;
      end loop;
   end Split;

begin
   if Ada.Command_Line.Argument_Count /= 1 then
      TIO.Put_Line (TIO.Standard_Error, "usage: qoi_driver <directory>");
      Ada.Command_Line.Set_Exit_Status (Ada.Command_Line.Failure);
      return;
   end if;

   declare
      Dir      : constant String := Ada.Command_Line.Argument (1);
      Manifest : TIO.File_Type;
   begin
      TIO.Open (Manifest, TIO.In_File, Dir & "/manifest.txt");
      while not TIO.End_Of_File (Manifest) loop
         declare
            Line   : constant String := TIO.Get_Line (Manifest);
            Tokens : Token_Array;
            Count  : Natural;

            function Tok (N : Positive) return String is
              (Line (Tokens (N).First .. Tokens (N).Last));
         begin
            Split (Line, Tokens, Count);
            if Count >= 2 then
               begin
                  if Tok (1) = "E" and then Count >= 6 then
                     Encode_Case
                       (Dir, Tok (2),
                        Storage_Count'Value (Tok (3)),
                        Storage_Count'Value (Tok (4)),
                        Storage_Count'Value (Tok (5)),
                        Natural'Value (Tok (6)));
                  elsif Tok (1) = "D" then
                     Decode_Case (Dir, Tok (2));
                  end if;
                  Done := Done + 1;
               exception
                  when E : others =>
                     Failures := Failures + 1;
                     TIO.Put_Line
                       (TIO.Standard_Error,
                        "case " & Tok (2) & ": "
                        & Ada.Exceptions.Exception_Information (E));
               end;
            end if;
         end;
      end loop;
      TIO.Close (Manifest);
   end;

   TIO.Put_Line
     ("QOI_ADA_DRIVER_DONE " & Image (Long_Long_Integer (Done))
      & " cases, " & Image (Long_Long_Integer (Failures)) & " exceptions");
   if Failures > 0 then
      Ada.Command_Line.Set_Exit_Status (Ada.Command_Line.Failure);
   end if;
end QOI_Driver;
